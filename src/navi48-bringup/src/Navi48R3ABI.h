//
//  Navi48R3ABI.h — kernel/userspace contract for the RDNA3 (Phoenix 780M)
//  stage-3 user ABI: BO alloc (UMA / GTT), BO free, IB submit, fence read.
//
//  Included by BOTH the kext (rdna3/NaviR3UserClient.cpp) and the userspace
//  harness (tools/pc/n48r3test.c) — one definition of every selector, struct
//  and validation rule.  C-core (no IOKit types); comments use // as in the
//  sibling Navi48UserClientABI.h.
//
//  Shape (fixed by C-10/实施文档 §11, 裁决文档 §2.2/§2.4; the pre-R1 header
//  declared a different, unused ABI and is replaced by this one):
//    - selector 2 BOAlloc:  in scalar  [size, domain]  out scalar [gpu_va lo, hi, handle]
//    - selector 3 BOFree:   in scalar  [gpu_va lo, hi]
//    - selector 4 Submit:   structure in N48R3CsIn (header + N48R3CsIb[1..64])
//                           out scalar [fence lo, hi]
//    - selector 5 FenceRead: in scalar [seq, timeout_us] out scalar [observed lo, hi, timed_out]

//  GPU address rules follow the stage-2 precedent (裁决文档 §2.2 / §2.4; S13):
//  - UMA BO: canonical GPU VA = canonicalize(fb_mc_base + D7 区偏移 + 页内偏移)。
//    The bare 48-bit MC form (fb_mc_base + off) is what goes inside PM4 packets
//    (INDIRECT_BUFFER canonicalizes internally; WRITE_DATA/RELEASE_MEM carry the
//    raw MC address — pm4_v11.cpp:46-59/:78-102).
//  - canonical = bits 63:47 all equal (native_s1b_pure.h:60-65 precedent).
//  - GTT BO: GPU VA = GART VA (low half, already canonical).
//
//  GPU address semantics (裁决文档 §2.2; S13 活对照 compute_v11.cpp:159-263):
//    - UMA BO:  GPU VA = fb_mc_base + D7 区偏移 + 页内偏移（MC aperture 地址）。
//               一切 io->bus 调用只传 carveout 字节偏移（grant->start+off）；
//               fb_mc_base 只进入 PM4 包内容（INDIRECT_BUFFER 基址、WRITE_DATA
//               目标、RELEASE_MEM fence 目标）。
//    - GTT BO:  GPU VA = GART VA（gmc 既有 GART 表 PTE 编程），CPU 侧为
//               系统内存物理页；其 CP 消费路径留阶段 4/5（见 NaviR3Bringup.cpp）。
//
//  CPU 访问（A-07 / C-04）：经 IOConnectMapMemory64(conn, handle) 映射——
//    UMA BO = BAR0 窗口切片（clientMemoryForType，Navi48UserClient.cpp:31-37 先例）；
//    GTT BO = 系统内存页。D6 fence_ro 共享页用魔数 kN48R3MemoryTypeFenceRo 映射
//    （内核每次提交写 fence seq，用户态只读轮询）。
//
//  稳定性：bring-up 接口，selector 编号为本工具与 kext 共同演进；头本身是
//  唯一权威（不得在 .cpp 内另立一套同义编号）。
//
#ifndef N48_R3_ABI_H
#define N48_R3_ABI_H

#include <stdint.h>

#define N48R3_UC_SERVICE_NAME "NaviR3Bringup"
#define N48R3_UC_ABI_VERSION  2u

/* ---- selector 编号（kext 的 NaviR3UserClient.cpp 与工具共同 include） ---- */
enum N48R3Selector {
    kN48R3SelInfo       = 0,   /* out struct: survey text（offset 分块读，既有） */
    kN48R3SelTrigger    = 1,   /* 触发 power-up substeps（既有） */

    /* T-E 阶段 3 selectors（R1 定案形态） */
    kN48R3SelBoAlloc    = 2,   /* in: size(8B), domain(8B)   out: gpu_va(lo,hi), handle */
    kN48R3SelBoFree     = 3,   /* in: gpu_va(lo,hi) */
    kN48R3SelSubmit     = 4,   /* structure in: N48R3CsIn（cs_ib[1..64]） out: fence(lo,hi) */
    kN48R3SelFenceRead  = 5,   /* in: seq, timeout_us        out: observed(lo,hi), timed_out */
    /* 6-8 reserved */

    /* 既有 log selectors（继承自 Navi48） */
    kN48R3SelReadLog    = 10,
    kN48R3SelLogReset   = 11,
    kN48R3SelLogConsume = 12,

    /* 登记编号、未实现：返回 kIOReturnUnsupported */
    kN48R3SelBoImportHost = 21,

    /* Dbg selectors：登记编号、未实现 */
    kN48R3SelDbgSmu       = 100,
    kN48R3SelDbgPspRing   = 101,
    kN48R3SelDbgVramRead  = 102,
    kN48R3SelDbgPspSubmit = 103,
    kN48R3SelDbgFwStage   = 104,
    kN48R3SelDbgPspLoadIpFw = 105,
    kN48R3SelDbgPspCtl    = 106,
    kN48R3SelDbgPspRaw    = 107,
    kN48R3SelDbgWriteReg  = 108,
    kN48R3SelDbgVramWrite = 109,
    kN48R3SelDbgAdoptQ    = 112,
    kN48R3SelDbgClaim     = 113,
};

/* ---- BO 域 ---- */
enum N48R3BoDomain {
    kN48R3DomainUMA = 0,   /* D7 区 BO：GPU VA = fb_mc_base + D7 偏移（MC aperture） */
    kN48R3DomainGTT = 1,   /* GART BO：GPU VA = GART VA（系统内存物理页 + GART PTE） */
};

/* ---- IP 类型（N48R3CsIb.ip_type 取值） ---- */
enum N48R3IpType {
    kN48R3IpGfx     = 0,   /* GFX ring；本阶段仅计算队列被实现 */
    kN48R3IpCompute = 1,   /* MES 管理的 compute queue（本阶段提交目标） */
};

/* ---- clientMemoryForType 专用映射类型（不与 BO handle 1..64 冲突） ---- */
#define kN48R3MemoryTypeFenceRo 0x44563030u  /* "DV00"：D6 fence_ro 共享只读页 */

/* BO handle 上限（BO 池容量；handle 1..N48R3_BO_MAX，同时是
 * IOConnectMapMemory64 的 memoryType，见 NaviR3Bringup.cpp 的 BOEntry 池）。 */
#define N48R3_BO_MAX 64u

/* compute ring 回绕填充/测试 IB 用的 NO_OP 头。PACKET3(PACKET3_NOP, 0) =
 * TYPE3<<30 | 0x10<<8 | 0<<16 = 0xC0001000（nvd.h:48-55：op=0x10，count=0）。
 * count=0 的包长为 count+2 = 2 个 dword：完整 NO_OP 包 = [N48R3_PM4_NOP, 0]，
 * 即 N48R3_PM4_NOP_PAIR（与 R2 mes_v11_0.cpp:1753 的 0x00000000C0001000ull
 * 形态一致并通过 test_mes_api 模拟器锁定）。写 ring 时必须成对写。 */
#define N48R3_PM4_NOP       0xC0001000u          /* PACKET3(PACKET3_NOP, 0) 包头 */
#define N48R3_PM4_NOP_PAIR  0x00000000C0001000ull /* 完整 NO_OP 包：头 + body 0 */

/* ---- 提交（selector 4） ---- */
/* 每一条 cs_ib 描述一个 GPU 执行的命令缓冲：数据由用户经自己的 BO 映射写入，
 * 内核只构造 INDIRECT_BUFFER 指向它（绝不把 IB 数据拷进 ring，C-05）。 */
struct N48R3CsIb {
    uint64_t va_start;      /* IB 起点 GPU 地址：必须落在用户已分配的 BO 范围内 */
    uint32_t ib_bytes;      /* IB 字节数：4 对齐，<= 0xFFFFF*4 */
    uint32_t ip_type;       /* 0 = GFX, 1 = Compute；本阶段仅 Compute ring 被实现 */
};

struct N48R3CsIn {
    uint32_t     abi_version;  /* 必须 == N48R3_UC_ABI_VERSION */
    uint32_t     num_ibs;      /* 1..64 */
    uint32_t     flags;        /* 保留，0 */
    uint32_t     reserved;     /* 保留，0 */
    /* 后接 num_ibs × N48R3CsIb（总尺寸 = sizeof(N48R3CsIn) + num_ibs*sizeof(N48R3CsIb)） */
};

#if !defined(__cplusplus)
typedef struct N48R3CsIb N48R3CsIb;
typedef struct N48R3CsIn N48R3CsIn;
#endif

#define N48R3_CS_IB_MAX        64u
#define N48R3_CS_IB_BYTES_MAX  (0xFFFFFu * 4u)   /* INDIRECT_BUFFER 的 length 上限（pm4_v11） */

/* ---- 纯函数：地址形态与参数校验（kext / 工具 / host 测试共用） ----
 * host 测试（X-02）与 n48r3test 的 N48R3_HOST_TEST 直接 include 本头测试这些。 */
#if defined(__cplusplus)
extern "C" {
#endif

/* 48-bit VM address mask / canonical helpers (照 native_s1b_pure.h:60-70 形态；
 * C-05 要求 canonical 校验，见 n48r3_cs_ib_valid)。 */
#define N48R3_VA_MASK48 0x0000FFFFFFFFFFFFull
static inline uint64_t n48r3_va_strip(uint64_t va)
{
    return va & N48R3_VA_MASK48;
}
static inline int n48r3_va_canonical(uint64_t va)
{
    const uint64_t top = va >> 47;  /* 17 bits */
    return (top == 0ull || top == 0x1FFFFull) ? 1 : 0;
}
static inline uint64_t n48r3_va_canonicalize(uint64_t va48)
{
    va48 &= N48R3_VA_MASK48;
    return (va48 & (1ull << 47)) ? (va48 | ~N48R3_VA_MASK48) : va48;
}
/* 把 scalar lo/hi 拼成 64 位 GPU 地址。 */
static inline uint64_t n48r3_va_from_scalars(uint64_t lo, uint64_t hi)
{
    return (hi << 32) | (lo & 0xFFFFFFFFull);
}

/* UMA BO 的 GPU 地址形态：fb_mc_base + D7 区偏移 + 页内偏移（MC aperture 地址）。 */
static inline uint64_t n48r3_uma_gpu_va(uint64_t fb_mc_base, uint64_t d7_off)
{
    return fb_mc_base + d7_off;
}

/* UMA BO 的 canonical 形 GPU VA（doBOAlloc 返回此值；INDIRECT_BUFFER 直接使用）。 */
static inline uint64_t n48r3_uma_gpu_va_canonical(uint64_t fb_mc_base,
                                                   uint64_t d7_off)
{
    return n48r3_va_canonicalize(n48r3_uma_gpu_va(fb_mc_base, d7_off));
}

/* io->bus 应收到 carveout 字节偏移（grant->start+off），而不是 MC 地址。
 * 校验 MC 地址落在 fb_mc_base 内并返回其 carveout 偏移；fb_mc_base 未就绪
 * （0）或地址不在 aperture 内拒绝（返回 0）——负向测试依赖此语义。 */
static inline uint64_t n48r3_carveout_offset(uint64_t fb_mc_base, uint64_t mc_addr,
                                             uint64_t size)
{
    const uint64_t mc = n48r3_va_strip(mc_addr);   /* canonical 或 raw 48 位均可 */
    if (fb_mc_base == 0ull || mc < fb_mc_base || mc + size < mc)
        return 0;
    (void)size;
    return mc - fb_mc_base;
}

/* cs_in 总字节数（校验用）。 */
static inline uint32_t n48r3_cs_in_size(const struct N48R3CsIn *in)
{
    return sizeof(struct N48R3CsIn) + in->num_ibs * sizeof(struct N48R3CsIb);
}

/* 单条 IB 校验：4 对齐、长度上限、IP 类型合法（本阶段仅 Compute=1 被实现）。
 * 返回 0 合法，非 0 非法。 */
static inline int n48r3_cs_ib_valid(const struct N48R3CsIb *ib)
{
    if (!ib) return 1;
    if (ib->va_start == 0) return 1;
    if ((ib->ib_bytes & 3u) != 0 || ib->ib_bytes == 0 ||
        ib->ib_bytes > N48R3_CS_IB_BYTES_MAX) return 1;
    if (ib->ip_type != 0u && ib->ip_type != 1u) return 1;
    /* canonical 校验（C-05；native_s1b_pure.h:60-65 形态）：
     * 地址必须在 48 位空间内（低半或高半）。 */
    if (n48r3_va_canonical(ib->va_start) == 0) return 1;
    return 0;
}

/* 提交参数解析 + 静态校验（地址范围由 kext 依 BO 注册表复查，见
 * NaviR3UserClient::doSubmit）：
 *   buf/size 为 structureInput；out 填 num_ibs 与每个 IB。
 * 返回 0 合法；-1 参数非法（size/abi/num_ibs），-2 某条 IB 非法。 */
static inline int n48r3_cs_in_validate(const void *buf, uint32_t size,
                                       const struct N48R3CsIn **header,
                                       const struct N48R3CsIb **ibs)
{
    if (!buf || size < sizeof(struct N48R3CsIn)) return -1;
    const struct N48R3CsIn *in = (const struct N48R3CsIn *)buf;
    if (in->abi_version != N48R3_UC_ABI_VERSION) return -1;
    if (in->num_ibs < 1u || in->num_ibs > N48R3_CS_IB_MAX) return -1;
    if (in->flags != 0u || in->reserved != 0u) return -1;
    if (size != n48r3_cs_in_size(in)) return -1;
    const struct N48R3CsIb *list = (const struct N48R3CsIb *)((const char *)buf + sizeof(*in));
    for (uint32_t i = 0; i < in->num_ibs; i++)
        if (n48r3_cs_ib_valid(&list[i]) != 0) return -2;
    if (header) *header = in;
    if (ibs) *ibs = list;
    return 0;
}
/* BOAlloc 参数校验（selector 2）：size (0, 1 MiB]、domain 合法。
 * 返回 0 合法，-1 非法。 */
static inline int n48r3_bo_alloc_args(uint64_t size, uint64_t domain)
{
    if (size == 0 || size > 0x100000ull) return -1;
    if (domain != (uint64_t)kN48R3DomainUMA && domain != (uint64_t)kN48R3DomainGTT)
        return -1;
    return 0;
}

/* WRITE_DATA 包构造（compute 子命令的用户 IB 用；值照 pm4_v11.cpp:46-59 的
 * golden 形态：0xC0370003 头 / 0x00100500 控制字 / dst lo / dst hi / data）。
 * dst 为 48 位 MC/VM 地址（用户侧用 n48r3_va_strip(gpu_va) 得到）。 */
static inline int n48r3_pm4_write_data(uint32_t out[5], uint64_t dst_va48,
                                       uint32_t data)
{
    if (!out || (dst_va48 & 3ull) != 0ull) return -1;
    out[0] = 0xC0370003u;
    out[1] = 0x00100500u;
    out[2] = (uint32_t)(dst_va48 & 0xFFFFFFFFull);
    out[3] = (uint32_t)(dst_va48 >> 32);
    out[4] = data;
    return 5;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* N48_R3_ABI_H */