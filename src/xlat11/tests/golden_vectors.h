/* golden_vectors.h - HAND-WRITTEN minimal vectors for src/xlat11 (Decision D10).
 *
 * Expected dwords are written out here from the authoritative register databases
 * (Mesa gfx103.json / gfx11.json field lists and the Linux gc_10_3_0 / gc_11_0_0
 * offsets), NOT read back from the translator or its generated table. They are
 * the independent oracle the unit tests compare against (requirement H5).
 *
 * Input is Apple-authored gfx10.3 PM4 (same dialect as src/xlat12): a type-3
 * header, then body. SET_* bodies are an offset dword (dword within the block)
 * followed by values. Offsets used below:
 *   0x28800 DB_DEPTH_CONTROL / 0x28808 CB_COLOR_CONTROL / 0x2880c DB_SHADER_CONTROL
 *          context dword offsets 0x200 / 0x202 / 0x203
 *   0x0b8ac COMPUTE_RELAUNCH            SH offset 0x22b
 *   0x28ee0 CB_COLOR0_ATTRIB3          context offset 0x3b8
 *   0x28b54 VGT_SHADER_STAGES_EN       context offset 0x2d5 (PRIMGEN_EN = bit 13)
 *   0x28424 CB_DCC_CONTROL -> FDCC     context offset 0x109 (reused)
 *   0x28b94 VGT_STRMOUT_CONFIG         context offset 0x2e5 (absent on gfx11)
 *   0x0b120 SPI_SHADER_PGM_LO_VS       SH offset 0x48 (legacy VS)
 */
#ifndef XLAT11_GOLDEN_VECTORS_H
#define XLAT11_GOLDEN_VECTORS_H

#include <stdint.h>

typedef struct {
    const char *name;
    const uint32_t *in;
    uint32_t in_len;
    const uint32_t *exp;       /* only checked when status == "OK" */
    uint32_t exp_len;
    const char *status;
    uint32_t err_addr;         /* only checked when status != "OK" */
    uint32_t ngg_in;           /* seed ctx.vs_mode = NGG */
    uint32_t ngg_out;          /* expect ctx.vs_mode == NGG after */
    /* counters */
    uint32_t regs_in, identical, moved, repacked, dropped_absent, dropped_legacy, regs_reused;
    uint32_t set_packets_out, set_runs_split;
} GoldenVec;

/* 1. identical + repack, three SET_CONTEXT_REG packets (0x28804 is not tabled, so a
 * single contiguous run would stop there):
 *   DB_DEPTH_CONTROL 0x28800 = 0x11111111 identical
 *   CB_COLOR_CONTROL 0x28808 = 0x22222222 identical
 *   DB_SHADER_CONTROL 0x2880c = 0xffffffff repacked over its listed gfx11 fields
 *   (bits 0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,23) = 0x0081fff7. */
static const uint32_t kIdenticalIn[]  = { 0xC0016900u, 0x200u, 0x11111111u,
                                          0xC0016900u, 0x202u, 0x22222222u,
                                          0xC0016900u, 0x203u, 0xFFFFFFFFu };
static const uint32_t kIdenticalExp[] = { 0xC0016900u, 0x200u, 0x11111111u,
                                          0xC0016900u, 0x202u, 0x22222222u,
                                          0xC0016900u, 0x203u, 0x0081fff7u };

/* 2. moved with identical fields: COMPUTE_RELAUNCH 0x0b8ac -> 0x0b8c0 (same value). */
static const uint32_t kMovedIn[]  = { 0xC0017600u, 0x22Bu, 0xDEADBEEFu };
static const uint32_t kMovedExp[] = { 0xC0017600u, 0x230u, 0xDEADBEEFu };

/* 3. field_repack: CB_COLOR0_ATTRIB3 0x02014678. gfx11 CB_COLOR0_ATTRIB3 keeps by name
 * MIP0_DEPTH [12:0]=0x678, COLOR_SW_MODE [18:14]=5, RESOURCE_TYPE [25:24]=2; the other
 * gfx10.3 bits are dropped, so the value is unchanged at 0x02014678 (5 = binary 00101
 * sets bits 14 and 16). Expected 0x02014678. */
static const uint32_t kRepackIn[]  = { 0xC0016900u, 0x3B8u, 0x02014678u };
static const uint32_t kRepackExp[] = { 0xC0016900u, 0x3B8u, 0x02014678u };

/* 4. NGG refusal: VGT_SHADER_STAGES_EN with PRIMGEN_EN (bit 13) = 0. */
static const uint32_t kNggRejectIn[] = { 0xC0016900u, 0x2D5u, 0x00000000u };

/* 5. NGG accept: PRIMGEN_EN = 1; VGT_SHADER_STAGES_EN is field_repack and keeps bit 13. */
static const uint32_t kNggAcceptIn[]  = { 0xC0016900u, 0x2D5u, 0x00002000u };
static const uint32_t kNggAcceptExp[] = { 0xC0016900u, 0x2D5u, 0x00002000u };

/* 6. reused: CB_DCC_CONTROL 0x00005555 -> gfx11 CB_FDCC_CONTROL at the SAME address.
 * Shared fields kept are bits 8,9,10,12,13,14; bits 0 and 2..6 (overwrite combiner)
 * are gfx10.3-only and dropped, so 0x5555 -> 0x5500. */
static const uint32_t kReusedIn[]  = { 0xC0016900u, 0x109u, 0x00005555u };
static const uint32_t kReusedExp[] = { 0xC0016900u, 0x109u, 0x00005500u };

/* 7. absent: VGT_STRMOUT_CONFIG has no gfx11 register; the packet emits nothing. */
static const uint32_t kAbsentIn[] = { 0xC0016900u, 0x2E5u, 0x00000001u };

/* 8. legacy VS before NGG is known: SPI_SHADER_PGM_LO_VS non-zero is refused. */
static const uint32_t kLegacyVsIn[] = { 0xC0017600u, 0x048u, 0x00001234u };

static const GoldenVec kGolden[] = {
    { "identical_run", kIdenticalIn, 9, kIdenticalExp, 9, "OK", 0, 0, 0,
      3, 2, 0, 1, 0, 0, 0, 3, 0 },
    { "moved", kMovedIn, 3, kMovedExp, 3, "OK", 0, 0, 0,
      1, 0, 1, 0, 0, 0, 0, 1, 0 },
    { "repack", kRepackIn, 3, kRepackExp, 3, "OK", 0, 0, 0,
      1, 0, 0, 1, 0, 0, 0, 1, 0 },
    { "ngg_reject", kNggRejectIn, 3, 0, 0, "ERR_LEGACY_VS", 0x28B54u, 0, 0,
      1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "ngg_accept", kNggAcceptIn, 3, kNggAcceptExp, 3, "OK", 0, 0, 1,
      1, 0, 0, 1, 0, 0, 0, 1, 0 },
    { "reused", kReusedIn, 3, kReusedExp, 3, "OK", 0, 0, 0,
      1, 0, 0, 0, 0, 0, 1, 1, 0 },
    { "absent_drop", kAbsentIn, 3, 0, 0, "OK", 0, 0, 0,
      1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { "legacy_vs_refuse", kLegacyVsIn, 3, 0, 0, "ERR_VS_MODE_UNKNOWN", 0x0B120u, 0, 0,
      1, 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define GOLDEN_COUNT ((uint32_t)(sizeof(kGolden) / sizeof(kGolden[0])))

#endif /* XLAT11_GOLDEN_VECTORS_H */
