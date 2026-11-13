/* xlat11 - gfx10.3 -> gfx11 render-state PM4 translator. Contract: xlat11.h.
 *
 * Kext constraints: no heap, no floating point, no libc calls beyond what a struct
 * copy may lower to; all working state is a few stack words.
 */
#include "xlat11.h"
#include "xlat11_tables.h"
#include "xlat11_reused.h"

/* PM4 type-3 header (Mesa src/amd/common/sid.h:276-289 @4519cc56):
 * [31:30] type=3, [29:16] count = body dwords - 1, [15:8] opcode, [7:0] flags
 * (bit0 predicate, bit1 shader type, bit2 reset filter cam). */
#define PKT_TYPE(h)    (((h) >> 30) & 0x3u)
#define PKT3_COUNT(h)  (((h) >> 16) & 0x3FFFu)
#define PKT3_OP(h)     (((h) >> 8) & 0xFFu)
#define PKT3_FLAGS(h)  ((h) & 0xFFu)
#define PKT3_MK(op, cnt, flags) \
    (0xC0000000u | (((uint32_t)(cnt) & 0x3FFFu) << 16) | (((uint32_t)(op) & 0xFFu) << 8) | ((flags) & 0xFFu))

/* opcodes: Mesa sid.h:41-227 */
#define OP_CLEAR_STATE             0x12u
#define OP_CONTEXT_CONTROL         0x28u
#define OP_LOAD_UCONFIG_REG        0x5Eu
#define OP_LOAD_SH_REG             0x5Fu
#define OP_LOAD_CONTEXT_REG        0x61u
#define OP_LOAD_SH_REG_INDEX       0x63u
#define OP_SET_CONTEXT_REG         0x69u
#define OP_SET_SH_REG              0x76u
#define OP_SET_UCONFIG_REG         0x79u
#define OP_SET_UCONFIG_REG_INDEX   0x7Au
#define OP_SET_SH_REG_INDEX        0x9Bu
#define OP_LOAD_CONTEXT_REG_INDEX  0x9Fu

/* register blocks, identical on gfx10.3 and gfx11: Mesa sid.h:18-23 */
#define CTX_BASE   0x28000u
#define CTX_DWORDS 0x2000u    /* 0x28000..0x30000 */
#define SH_BASE    0x0B000u
#define SH_DWORDS  0x0400u    /* 0x0B000..0x0C000 */
#define UCF_BASE   0x30000u
#define UCF_DWORDS 0x4000u    /* 0x30000..0x40000 */

#ifdef __cplusplus
static const xlat11_stats kZeroStats = {};
#else
static const xlat11_stats kZeroStats = {0};
#endif

static const Xlat11Entry *lookup(uint32_t g10)
{
    /* The REUSED override wins over the generated row for the same address. xlat11_reused.h
     * says why it is a separate hand-written file and not an edit inside the generated table. */
    const Xlat11Entry *ov = xlat11_reused_lookup(g10);
    if (ov) return ov;
    uint32_t lo = 0, hi = XLAT11_TABLE_LEN;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (kXlat11Table[mid].g10 == g10) return &kXlat11Table[mid];
        if (kXlat11Table[mid].g10 < g10) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int xlat11_lookup(uint32_t g10_addr, uint32_t *out_g11, uint32_t *out_cls)
{
    const Xlat11Entry *e = lookup(g10_addr);
    if (!e) return 0;
    if (out_g11) *out_g11 = e->g11;
    if (out_cls) *out_cls = e->cls;
    return 1;
}

uint32_t xlat11_repack_value(uint32_t g10_addr, uint32_t value)
{
    const Xlat11Entry *e = lookup(g10_addr);
    if (!e || !e->repack) return value;
    return (e->cls == XLAT11_CLS_FIELD_REPACK || e->cls == XLAT11_CLS_REUSED) ? e->repack(value) : value;
}

uint32_t xlat11_table_len(void) { return XLAT11_TABLE_LEN; }

int xlat11_table_entry(uint32_t i, uint32_t *g10, uint32_t *g11, uint32_t *cls, const char **name)
{
    if (i >= XLAT11_TABLE_LEN) return 0;
    /* Introspection reports what lookup() would answer, override included - the output verifier's
     * allow-list is built from exactly this walk, so a row the translator emits at must be visible
     * here or the verifier refuses our own write. */
    const Xlat11Entry *e = lookup(kXlat11Table[i].g10);
    if (!e) e = &kXlat11Table[i];
    if (g10) *g10 = kXlat11Table[i].g10;
    if (g11) *g11 = e->g11;
    if (cls) *cls = e->cls;
    if (name) *name = e->name;
    return 1;
}

const char *xlat11_status_name(xlat11_status s)
{
    switch (s) {
    case XLAT11_OK:                  return "OK";
    case XLAT11_ERR_ARG:             return "ERR_ARG";
    case XLAT11_ERR_BAD_PACKET:      return "ERR_BAD_PACKET";
    case XLAT11_ERR_TRUNCATED:       return "ERR_TRUNCATED";
    case XLAT11_ERR_RANGE:           return "ERR_RANGE";
    case XLAT11_ERR_BAD_INDEX:       return "ERR_BAD_INDEX";
    case XLAT11_ERR_UNKNOWN_REG:     return "ERR_UNKNOWN_REG";
    case XLAT11_ERR_LEGACY_VS:       return "ERR_LEGACY_VS";
    case XLAT11_ERR_VS_MODE_UNKNOWN: return "ERR_VS_MODE_UNKNOWN";
    case XLAT11_ERR_MEMLOADED:       return "ERR_MEMLOADED";
    case XLAT11_ERR_CAPACITY:        return "ERR_CAPACITY";
    }
    return "?";
}

static uint32_t set_op_for_addr(uint32_t a)
{
    if (a >= CTX_BASE && a < CTX_BASE + (CTX_DWORDS << 2)) return OP_SET_CONTEXT_REG;
    if (a >= SH_BASE  && a < SH_BASE  + (SH_DWORDS  << 2)) return OP_SET_SH_REG;
    if (a >= UCF_BASE && a < UCF_BASE + (UCF_DWORDS << 2)) return OP_SET_UCONFIG_REG;
    return 0;
}

static void block_of_op(uint32_t op, uint32_t *base, uint32_t *dwords)
{
    switch (op) {
    case OP_SET_CONTEXT_REG:       *base = CTX_BASE; *dwords = CTX_DWORDS; break;
    case OP_SET_SH_REG:
    case OP_SET_SH_REG_INDEX:      *base = SH_BASE;  *dwords = SH_DWORDS;  break;
    case OP_SET_UCONFIG_REG:
    case OP_SET_UCONFIG_REG_INDEX: *base = UCF_BASE; *dwords = UCF_DWORDS; break;
    default:                       *base = 0;        *dwords = 0;          break;
    }
}

static const char *memloaded_name(uint32_t op)
{
    switch (op) {
    case OP_LOAD_CONTEXT_REG:       return "LOAD_CONTEXT_REG";
    case OP_LOAD_SH_REG:            return "LOAD_SH_REG";
    case OP_LOAD_UCONFIG_REG:       return "LOAD_UCONFIG_REG";
    case OP_LOAD_SH_REG_INDEX:      return "LOAD_SH_REG_INDEX";
    case OP_LOAD_CONTEXT_REG_INDEX: return "LOAD_CONTEXT_REG_INDEX";
    case OP_CLEAR_STATE:            return "CLEAR_STATE";
    case OP_CONTEXT_CONTROL:        return "CONTEXT_CONTROL";
    default:                        return 0;
    }
}

/* Index values accepted for the _INDEX forms. This acceptance set is inherited
 * unchanged from xlat12 (Mesa amd/packets/cp_pm4_table_data_gfx12.json, SET_SH_REG_
 * INDEX :5961, SET_UCONFIG_REG_INDEX :6148: word 2 is reg_offset [15:0], reserved
 * [27:16], index [31:28]); the gfx11 packet table is a stage-3 review item, NOT a
 * stage-1 claim. SET_SH_REG_INDEX index 3 is apply_kmd_cu_and_mask.
 * SET_UCONFIG_REG_INDEX accepts 0 (default), 2 (index_type), 3 (num_instances), and
 * 1 (reserved in the gfx12 table but emitted by RADV for VGT_PRIMITIVE_TYPE on every
 * generation, radv_cmd_buffer.c:4683, and by Apple). */
static int index_allowed(uint32_t op, uint32_t idx)
{
    if (op == OP_SET_SH_REG_INDEX)
        return idx == 3u;
    return idx == 0u || idx == 1u || idx == 2u || idx == 3u;
}

/* ---- output emitter ------------------------------------------------------ */
typedef struct {
    uint32_t *out;
    uint32_t  cap;
    uint32_t  n;
    int       open;       /* a SET run is being built */
    uint32_t  op;
    uint32_t  flags;
    uint32_t  hdr_pos;
    uint32_t  next_off;
    uint32_t  count;
    uint32_t  runs_this_packet;
} Emitter;

static void run_close(Emitter *e, xlat11_stats *st)
{
    if (!e->open) return;
    e->out[e->hdr_pos] = PKT3_MK(e->op, e->count, e->flags);
    e->open = 0;
    st->packets_out++;
    st->set_packets_out++;
}

static xlat11_status run_add(Emitter *e, xlat11_stats *st, uint32_t g11, uint32_t flags, uint32_t val)
{
    uint32_t op = set_op_for_addr(g11), base, dwords;
    if (!op) return XLAT11_ERR_RANGE;
    block_of_op(op, &base, &dwords);
    uint32_t off = (g11 - base) >> 2;
    if (!(e->open && e->op == op && e->next_off == off && e->flags == flags)) {
        run_close(e, st);
        if (e->cap - e->n < 3u) return XLAT11_ERR_CAPACITY;   /* header, offset, value */
        e->hdr_pos = e->n;
        e->out[e->n++] = 0;                                   /* patched at close */
        e->out[e->n++] = off;
        e->open = 1; e->op = op; e->flags = flags; e->count = 0;
        e->runs_this_packet++;
    } else if (e->cap - e->n < 1u) {
        return XLAT11_ERR_CAPACITY;
    }
    e->out[e->n++] = val;
    e->count++;
    e->next_off = off + 1u;
    return XLAT11_OK;
}

/* Decide one register value. Returns OK and *emit=1 with *val set when the value
 * must be written at ent->g11; OK and *emit=0 when dropped; an error otherwise. */
static xlat11_status decide(xlat11_ctx *ctx, xlat11_stats *st, uint32_t g10, uint32_t *val, int *emit,
                            const Xlat11Entry **pent)
{
    const Xlat11Entry *ent = lookup(g10);
    *emit = 0; *pent = ent;
    st->regs_in++;
    if (!ent || ent->cls == XLAT11_CLS_UNKNOWN) {
        st->err_reg_addr = g10;
        st->err_name = ent ? ent->name : "UNTABLED_GFX10_ADDRESS";
        return XLAT11_ERR_UNKNOWN_REG;
    }
    switch (ent->cls) {
    case XLAT11_CLS_ABSENT:
        st->regs_dropped_absent++;
        return XLAT11_OK;
    case XLAT11_CLS_LEGACY_VS:
        if ((ctx && ctx->vs_mode == XLAT11_VS_NGG) || *val == 0u) {
            st->regs_dropped_legacy_vs++;
            return XLAT11_OK;
        }
        st->err_reg_addr = g10; st->err_name = ent->name;
        return XLAT11_ERR_VS_MODE_UNKNOWN;
    case XLAT11_CLS_IDENTICAL:
    case XLAT11_CLS_MOVED:
    case XLAT11_CLS_FIELD_REPACK:
    case XLAT11_CLS_REUSED:
        if (g10 == XLAT11_G10_VGT_SHADER_STAGES_EN) {
            if (((*val >> XLAT11_G10_PRIMGEN_EN_BIT) & 1u) == 0u) {
                st->err_reg_addr = g10; st->err_name = ent->name;
                return XLAT11_ERR_LEGACY_VS;
            }
            if (ctx) ctx->vs_mode = XLAT11_VS_NGG;
        }
        if (ent->cls == XLAT11_CLS_REUSED) {
            *val = ent->repack(*val);          /* a DIFFERENT gfx11 register lives here: xlat11_reused.h */
            st->regs_reused++;
        } else if (ent->cls == XLAT11_CLS_FIELD_REPACK) {
            *val = ent->repack(*val);
            st->regs_repacked++;
        } else if (ent->cls == XLAT11_CLS_MOVED) {
            st->regs_moved++;
        } else {
            st->regs_identical++;
        }
        *emit = 1;
        return XLAT11_OK;
    default:
        st->err_reg_addr = g10; st->err_name = ent->name;
        return XLAT11_ERR_UNKNOWN_REG;
    }
}

xlat11_status xlat11_translate(xlat11_ctx *ctx, const uint32_t *in, uint32_t in_dwords,
                               uint32_t *out, uint32_t out_cap,
                               uint32_t *out_dwords, xlat11_stats *stats)
{
    xlat11_stats local;
    xlat11_stats *st = stats ? stats : &local;
    *st = kZeroStats;
    if (out_dwords) *out_dwords = 0;
    if ((!in && in_dwords) || (!out && out_cap)) return XLAT11_ERR_ARG;

    Emitter e;
    e.out = out; e.cap = out_cap; e.n = 0; e.open = 0; e.op = 0; e.flags = 0;
    e.hdr_pos = 0; e.next_off = 0; e.count = 0; e.runs_this_packet = 0;

    uint32_t i = 0;
    while (i < in_dwords) {
        uint32_t hdr = in[i];
        st->err_in_dword = i;
        if (PKT_TYPE(hdr) != 3u) { st->err_opcode = hdr; return XLAT11_ERR_BAD_PACKET; }
        uint32_t op = PKT3_OP(hdr), flags = PKT3_FLAGS(hdr);
        uint32_t body = PKT3_COUNT(hdr) + 1u;                 /* <= 0x4000 */
        if ((uint64_t)i + 1u + body > (uint64_t)in_dwords) { st->err_opcode = op; return XLAT11_ERR_TRUNCATED; }
        const uint32_t *p = &in[i + 1];
        st->packets_in++;

        const char *ml = memloaded_name(op);
        if (ml) { st->err_opcode = op; st->err_name = ml; return XLAT11_ERR_MEMLOADED; }

        if (op == OP_SET_CONTEXT_REG || op == OP_SET_SH_REG || op == OP_SET_UCONFIG_REG) {
            uint32_t base, dwords, n = body - 1u;             /* body[0] is the offset word */
            block_of_op(op, &base, &dwords);
            st->set_packets_in++;
            if ((uint64_t)p[0] + n > dwords) { st->err_opcode = op; return XLAT11_ERR_RANGE; }
            e.runs_this_packet = 0;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t g10 = base + ((p[0] + k) << 2), val = p[1 + k];
                const Xlat11Entry *ent; int emit;
                xlat11_status s = decide(ctx, st, g10, &val, &emit, &ent);
                if (s != XLAT11_OK) { st->err_opcode = op; return s; }
                if (!emit) continue;
                s = run_add(&e, st, ent->g11, flags, val);
                if (s != XLAT11_OK) { st->err_opcode = op; st->err_reg_addr = g10; return s; }
            }
            run_close(&e, st);                              /* runs never span input packets */
            if (e.runs_this_packet == 0) st->set_packets_empty++;
            else st->set_runs_split += e.runs_this_packet - 1u;
            i += 1u + body;
            continue;
        }

        if (op == OP_SET_SH_REG_INDEX || op == OP_SET_UCONFIG_REG_INDEX) {
            uint32_t base, dwords, n = body - 1u;
            uint32_t idx = (p[0] >> 28) & 0xFu, off = p[0] & 0xFFFFu;
            block_of_op(op, &base, &dwords);
            st->set_packets_in++;
            if ((p[0] & 0x0FFF0000u) != 0u || !index_allowed(op, idx)) {
                st->err_opcode = op; st->err_reg_addr = base + (off << 2);
                return XLAT11_ERR_BAD_INDEX;
            }
            if ((uint64_t)off + n > dwords) { st->err_opcode = op; return XLAT11_ERR_RANGE; }
            uint32_t emitted = 0;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t g10 = base + ((off + k) << 2), val = p[1 + k];
                const Xlat11Entry *ent; int emit;
                xlat11_status s = decide(ctx, st, g10, &val, &emit, &ent);
                if (s != XLAT11_OK) { st->err_opcode = op; return s; }
                if (!emit) continue;
                uint32_t b2, d2;
                block_of_op(op, &b2, &d2);
                if (set_op_for_addr(ent->g11) != (op == OP_SET_SH_REG_INDEX ? OP_SET_SH_REG : OP_SET_UCONFIG_REG)) {
                    st->err_opcode = op; st->err_reg_addr = g10; return XLAT11_ERR_RANGE;
                }
                if (e.cap - e.n < 3u) { st->err_opcode = op; st->err_reg_addr = g10; return XLAT11_ERR_CAPACITY; }
                e.out[e.n++] = PKT3_MK(op, 1u, flags);
                e.out[e.n++] = (idx << 28) | ((ent->g11 - b2) >> 2);
                e.out[e.n++] = val;
                st->packets_out++; st->set_packets_out++; emitted++;
            }
            if (emitted == 0) st->set_packets_empty++;
            else st->set_runs_split += emitted - 1u;
            i += 1u + body;
            continue;
        }

        /* Any other type-3 packet is copied verbatim. A SET translation changes IB
         * length, so COND_EXEC skip counts and IB sizes around this stream must be
         * recomputed by the integration layer. */
        if ((uint64_t)e.cap - e.n < (uint64_t)1u + body) { st->err_opcode = op; return XLAT11_ERR_CAPACITY; }
        for (uint32_t k = 0; k < 1u + body; k++) e.out[e.n++] = in[i + k];
        st->packets_out++; st->packets_verbatim++;
        i += 1u + body;
    }

    st->err_in_dword = 0;
    st->dwords_out = e.n;
    if (out_dwords) *out_dwords = e.n;
    return XLAT11_OK;
}
