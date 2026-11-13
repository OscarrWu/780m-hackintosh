/* Host tests for src/xlat11. Build and run: make -C src/xlat11 test
 *
 * Expected values come from tests/golden_vectors.h (hand-derived from the Mesa
 * field lists / Linux offsets) and from hand cases written out here, never from
 * the translator's own output (requirement H5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat11.h"
#include "golden_vectors.h"
/* The raw generated table, to assert the generated ABSENT row a REUSED override
 * supersedes. The repack_* functions it references are "used" through the table's
 * own initializer, so -Wunused-function does not fire. */
#include "xlat11_tables.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CAP 16384u
static uint32_t g_out[CAP];

/* ---- 1. golden vectors --------------------------------------------------- */
static void test_golden(void)
{
    for (uint32_t v = 0; v < GOLDEN_COUNT; v++) {
        const GoldenVec *g = &kGolden[v];
        xlat11_ctx ctx; ctx.vs_mode = g->ngg_in ? XLAT11_VS_NGG : XLAT11_VS_UNKNOWN;
        xlat11_stats st; uint32_t n = 0;
        xlat11_status s = xlat11_translate(&ctx, g->in, g->in_len, g_out, CAP, &n, &st);
        const char *sn = xlat11_status_name(s);
        CHECK(strcmp(sn, g->status) == 0, "%s: status %s, oracle %s (err_addr 0x%x name %s)", g->name, sn,
              g->status, st.err_reg_addr, st.err_name ? st.err_name : "-");
        CHECK(st.regs_in == g->regs_in && st.regs_identical == g->identical && st.regs_moved == g->moved &&
              st.regs_repacked == g->repacked && st.regs_dropped_absent == g->dropped_absent &&
              st.regs_dropped_legacy_vs == g->dropped_legacy && st.regs_reused == g->regs_reused,
              "%s: counts in/id/mv/rp/abs/lvs/reu %u/%u/%u/%u/%u/%u/%u, oracle %u/%u/%u/%u/%u/%u/%u", g->name,
              st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked, st.regs_dropped_absent,
              st.regs_dropped_legacy_vs, st.regs_reused, g->regs_in, g->identical, g->moved, g->repacked,
              g->dropped_absent, g->dropped_legacy, g->regs_reused);
        if (s != XLAT11_OK) {
            CHECK(st.err_reg_addr == g->err_addr, "%s: err_addr 0x%x, oracle 0x%x", g->name, st.err_reg_addr, g->err_addr);
            printf("  golden %-18s %-20s at 0x%05x (%s) after %u regs\n", g->name, sn, st.err_reg_addr,
                   st.err_name ? st.err_name : "-", st.regs_in);
            continue;
        }
        CHECK(n == g->exp_len, "%s: out %u dwords, oracle %u", g->name, n, g->exp_len);
        uint32_t first_bad = UINT32_MAX;
        for (uint32_t i = 0; i < n && i < g->exp_len; i++)
            if (g_out[i] != g->exp[i]) { first_bad = i; break; }
        CHECK(first_bad == UINT32_MAX, "%s: dword %u = 0x%08x, oracle 0x%08x", g->name, first_bad,
              first_bad == UINT32_MAX ? 0 : g_out[first_bad], first_bad == UINT32_MAX ? 0 : g->exp[first_bad]);
        CHECK(st.set_packets_out == g->set_packets_out && st.set_runs_split == g->set_runs_split,
              "%s: set_out/split %u/%u, oracle %u/%u", g->name, st.set_packets_out, st.set_runs_split,
              g->set_packets_out, g->set_runs_split);
        CHECK((ctx.vs_mode == XLAT11_VS_NGG) == (g->ngg_out != 0), "%s: ngg_out", g->name);
        printf("  golden %-18s OK  in %2u -> out %3u dw; regs %u: id %u mv %u rp %u reused %u, dropped absent %u legacy-VS %u\n",
               g->name, g->in_len, n, st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked,
               st.regs_reused, st.regs_dropped_absent, st.regs_dropped_legacy_vs);
    }
}

/* ---- 2. class counts over the generated table ---------------------------- */
static void test_table_classes(void)
{
    uint32_t c_id = 0, c_mv = 0, c_rp = 0, c_abs = 0, c_lvs = 0, c_reu = 0, c_unk = 0, c_bad = 0;
    for (uint32_t i = 0; i < xlat11_table_len(); i++) {
        uint32_t g10, g11, cls;
        if (!xlat11_table_entry(i, &g10, &g11, &cls, 0)) { c_bad++; continue; }
        switch (cls) {
        case XLAT11_CLS_IDENTICAL: c_id++; break;
        case XLAT11_CLS_MOVED: c_mv++; break;
        case XLAT11_CLS_FIELD_REPACK: c_rp++; break;
        case XLAT11_CLS_ABSENT: c_abs++; break;
        case XLAT11_CLS_LEGACY_VS: c_lvs++; break;
        case XLAT11_CLS_REUSED: c_reu++; break;
        default: c_unk++; break;
        }
    }
    printf("  table_entry over %u rows: identical %u / moved %u / repack %u / absent %u / legacy_vs %u / reused %u / unknown %u\n",
           xlat11_table_len(), c_id, c_mv, c_rp, c_abs, c_lvs, c_reu, c_unk);
    /* gen_tables.py summary (xlat11_tables.h): identical 31, moved 2, field_repack 12,
     * absent 11, legacy_vs 7, unknown 0. The 9 reused rows supersede 9 of the 11
     * generated ABSENT rows, so introspection reports absent 2 and reused 9. */
    CHECK(xlat11_table_len() == 63, "table length %u, expected 63", xlat11_table_len());
    CHECK(c_id == 31 && c_mv == 2 && c_rp == 12 && c_abs == 2 && c_lvs == 7 && c_reu == 9 && c_unk == 0,
          "table_entry classes %u/%u/%u/%u/%u/%u/%u, expected 31/2/12/2/7/9/0", c_id, c_mv, c_rp, c_abs, c_lvs, c_reu, c_unk);
    CHECK(c_bad == 0, "table_entry failed for %u rows", c_bad);

    /* Raw generated table still carries all 11 ABSENT rows (the override does not
     * edit it). */
    uint32_t raw_abs = 0, raw_total = 0;
    for (uint32_t i = 0; i < XLAT11_TABLE_LEN; i++) {
        raw_total++;
        if (kXlat11Table[i].cls == XLAT11_CLS_ABSENT) raw_abs++;
    }
    CHECK(raw_total == 63 && raw_abs == 11, "raw table %u rows, %u absent (expected 63/11)", raw_total, raw_abs);
    /* every FIELD_REPACK row has a repack function */
    for (uint32_t i = 0; i < XLAT11_TABLE_LEN; i++)
        if (kXlat11Table[i].cls == XLAT11_CLS_FIELD_REPACK)
            CHECK(kXlat11Table[i].repack != 0, "field_repack 0x%x has no repack function", kXlat11Table[i].g10);
}

/* ---- 3. reused lookup priority ------------------------------------------- */
static void test_reused_priority(void)
{
    static const uint32_t kReusedAddrs[] = {
        0x28424u, 0x28c78u, 0x28cb4u, 0x28cf0u, 0x28d2cu, 0x28d68u, 0x28da4u, 0x28de0u, 0x28e1cu
    };
    for (unsigned k = 0; k < sizeof kReusedAddrs / sizeof kReusedAddrs[0]; k++) {
        uint32_t a = kReusedAddrs[k], g11 = 0, cls = 0xFF;
        CHECK(xlat11_lookup(a, &g11, &cls) == 1 && g11 == a && cls == XLAT11_CLS_REUSED,
              "reused 0x%05x: lookup g11 0x%x cls %u, expected REUSED at same address", a, g11, cls);
        int found = 0;
        for (uint32_t i = 0; i < XLAT11_TABLE_LEN; i++)
            if (kXlat11Table[i].g10 == a) { found = 1; CHECK(kXlat11Table[i].cls == XLAT11_CLS_ABSENT,
                "0x%05x: generated row should still be ABSENT, is %u", a, kXlat11Table[i].cls); break; }
        CHECK(found, "0x%05x: no generated row to supersede", a);
    }
    /* the translate path counts the reused write and emits the mapped value */
    uint32_t in[] = { 0xC0016900u, 0x109u, 0x00005555u };
    xlat11_stats st; uint32_t n = 0;
    CHECK(xlat11_translate(0, in, 3, g_out, CAP, &n, &st) == XLAT11_OK && st.regs_reused == 1 &&
          n == 3 && g_out[1] == 0x109u && g_out[2] == 0x00005500u,
          "reused translate: status/reused/n/val %u/%u/0x%x", st.regs_reused, n, n == 3 ? g_out[2] : 0);
}

/* ---- 4. address + value round trip over the whole table ------------------ */
static uint32_t set_op_for(uint32_t a)
{
    if (a >= 0x28000u && a < 0x30000u) return 0x69u;
    if (a >= 0x0B000u && a < 0x0C000u) return 0x76u;
    return 0x79u;
}
static uint32_t base_for(uint32_t op) { return op == 0x69u ? 0x28000u : op == 0x76u ? 0x0B000u : 0x30000u; }

static void test_roundtrip(void)
{
    uint32_t n_id = 0, n_mv = 0, n_rp = 0, n_drop = 0, n_unk = 0, n_reu = 0;
    for (uint32_t i = 0; i < xlat11_table_len(); i++) {
        uint32_t g10, g11, cls; const char *nm;
        xlat11_table_entry(i, &g10, &g11, &cls, &nm);
        uint32_t op = set_op_for(g10);
        uint32_t val = 0xA5A5A5A5u | (1u << 13);          /* PRIMGEN_EN set for the NGG rule */
        uint32_t in[3] = { 0xC0000000u | (1u << 16) | (op << 8), (g10 - base_for(op)) >> 2, val };
        xlat11_ctx ctx; ctx.vs_mode = XLAT11_VS_NGG;
        xlat11_stats st; uint32_t n;
        xlat11_status s = xlat11_translate(&ctx, in, 3, g_out, CAP, &n, &st);
        if (cls == XLAT11_CLS_UNKNOWN) {
            CHECK(s == XLAT11_ERR_UNKNOWN_REG && st.err_reg_addr == g10, "%s: unknown not refused", nm); n_unk++; continue;
        }
        if (cls == XLAT11_CLS_ABSENT || cls == XLAT11_CLS_LEGACY_VS) {
            CHECK(s == XLAT11_OK && n == 0, "%s: dropped reg emitted %u dw", nm, n); n_drop++; continue;
        }
        uint32_t oop = set_op_for(g11);
        CHECK(s == XLAT11_OK && n == 3, "%s: status %s n %u", nm, xlat11_status_name(s), n);
        if (s != XLAT11_OK || n != 3) continue;
        uint32_t got_addr = base_for(oop) + (g_out[1] << 2);
        CHECK(((g_out[0] >> 8) & 0xFFu) == oop && got_addr == g11, "%s: emitted op 0x%x addr 0x%x, table 0x%x", nm,
              (g_out[0] >> 8) & 0xFFu, got_addr, g11);
        if (cls == XLAT11_CLS_IDENTICAL) {
            CHECK(g11 == g10 && g_out[2] == val, "%s: identical round trip", nm); n_id++;
        } else if (cls == XLAT11_CLS_MOVED) {
            CHECK(g11 != g10 && g_out[2] == val, "%s: moved value changed", nm); n_mv++;
        } else if (cls == XLAT11_CLS_REUSED) {
            CHECK(g11 == g10 && g_out[2] == xlat11_repack_value(g10, val), "%s: reused mapping", nm); n_reu++;
        } else {
            CHECK(g_out[2] == xlat11_repack_value(g10, val), "%s: repack via translate", nm); n_rp++;
        }
    }
    printf("  round trip over %u table entries: identical %u, moved %u, repack %u, reused %u, dropped %u, refused %u\n",
           xlat11_table_len(), n_id, n_mv, n_rp, n_reu, n_drop, n_unk);
    CHECK(n_id == 31 && n_mv == 2 && n_rp == 12 && n_reu == 9 && n_drop == 9 && n_unk == 0,
          "round trip classes %u/%u/%u/%u/%u/%u", n_id, n_mv, n_rp, n_reu, n_drop, n_unk);
}

/* ---- 5. hand-derived field repacks (independent of the generated functions) */
static void test_repack_hand_cases(void)
{
    /* DB_SHADER_CONTROL gfx11 fields: bits 0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,23. */
    CHECK(xlat11_repack_value(0x2880Cu, 0xFFFFFFFFu) == 0x0081FFF7u, "DB_SHADER_CONTROL hand case");
    /* SPI_PS_INPUT_CNTL_0: bits [5:0],[9:8],[10],[11],[17],[18],[19],[20],[22:21],[23],[24],[25]
     * = 0x03FE0F3F. */
    CHECK(xlat11_repack_value(0x28644u, 0xFFFFFFFFu) == 0x03FE0F3Fu, "SPI_PS_INPUT_CNTL_0 hand case: 0x%08x",
          xlat11_repack_value(0x28644u, 0xFFFFFFFFu));
    /* VGT_SHADER_STAGES_EN: HS_EN bit2, GS_EN bit5, PRIMGEN_EN bit13 keep their bits. */
    CHECK(xlat11_repack_value(0x28B54u, (1u << 13) | (1u << 5) | (1u << 2)) == 0x2024u, "VGT_SHADER_STAGES_EN hand case");
    /* CB_COLOR0_ATTRIB: NUM_FRAGMENTS[16:15]->[1:0], FORCE_DST_ALPHA_1[17]->[2],
     * LIMIT_COLOR_FETCH_TO_256B_MAX[19]->[4]. Input bits 15,16,17,19 set -> 0x17. */
    CHECK(xlat11_repack_value(0x28C74u, (3u << 15) | (1u << 17) | (1u << 19)) == 0x17u, "CB_COLOR0_ATTRIB hand case");
    /* moved class is identity for the value. */
    CHECK(xlat11_repack_value(0x0B8ACu, 0xDEADBEEFu) == 0xDEADBEEFu, "COMPUTE_RELAUNCH moved value identity");
    /* reused global DCC: only shared bits 8,9,10,12,13,14 survive. */
    CHECK(xlat11_repack_value(0x28424u, 0x00005555u) == 0x00005500u, "CB_DCC_CONTROL reused hand case");
    /* reused colour DCC: INDEPENDENT_128B_BLOCKS [20]->[10], DCC_COMPRESS_DISABLE [22]->[23],
     * OVERWRITE_COMBINER_DISABLE [0] dropped. */
    CHECK(xlat11_repack_value(0x28C78u, (1u << 20) | (1u << 22) | 1u) == 0x00800400u, "CB_COLOR0_DCC_CONTROL reused hand case");
    printf("  hand repack cases checked\n");
}

/* ---- 6. splitting, cross-block, flags, empty SET, verbatim --------------- */
static void test_split_flags_verbatim(void)
{
    xlat11_stats st; uint32_t n;
    /* DB_Z_INFO 0x28040 and DB_STENCIL_INFO 0x28044 are contiguous and identical. */
    uint32_t in1[] = { 0xC0026900u, 0x10u, 0x11u, 0x22u };
    CHECK(xlat11_translate(0, in1, 4, g_out, CAP, &n, &st) == XLAT11_OK && st.set_packets_out == 1 &&
          st.set_runs_split == 0 && n == 4 && memcmp(g_out, in1, sizeof in1) == 0, "no-split DB_Z/STENCIL run");

    /* VGT_GS_OUT_PRIM_TYPE: context 0x28a6c -> UCONFIG 0x30998 (cross-block). */
    uint32_t in2[] = { 0xC0016900u, 0x29Bu, 0x5u };
    CHECK(xlat11_translate(0, in2, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 3 &&
          ((g_out[0] >> 8) & 0xFFu) == 0x79u && 0x30000u + (g_out[1] << 2) == 0x30998u && g_out[2] == 5u,
          "cross-block VGT_GS_OUT_PRIM_TYPE");

    /* header flag byte kept: SET_SH_REG with SHADER_TYPE (0x02), SPI_SHADER_PGM_LO_PS identical. */
    uint32_t in3[] = { 0xC0017602u, 0x08u, 0xDEAD0000u };
    CHECK(xlat11_translate(0, in3, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 3 && g_out[0] == 0xC0017602u &&
          g_out[1] == 0x08u && g_out[2] == 0xDEAD0000u, "flags: header 0x%08x", g_out[0]);

    /* empty SET (offset word only) and verbatim NOP / DRAW_INDEX_AUTO. A type-3 count is
     * body_dwords - 1, so a count-0 NOP still carries one payload dword. */
    uint32_t in4[] = { 0xC0006900u, 0xB4u, 0xC0001000u, 0xABCD1234u, 0xC0012D00u, 3u, 2u };
    CHECK(xlat11_translate(0, in4, 7, g_out, CAP, &n, &st) == XLAT11_OK && n == 5 &&
          st.set_packets_empty == 1 && st.packets_verbatim == 2 && memcmp(g_out, in4 + 2, 5 * sizeof(uint32_t)) == 0,
          "empty SET dropped, NOP/DRAW verbatim (n %u)", n);
}

/* ---- 7. NGG rule --------------------------------------------------------- */
static void test_ngg(void)
{
    xlat11_stats st; uint32_t n;
    xlat11_ctx ctx;

    uint32_t stages0[] = { 0xC0016900u, 0x2D5u, 0x0u };
    CHECK(xlat11_translate(0, stages0, 3, g_out, CAP, &n, &st) == XLAT11_ERR_LEGACY_VS, "PRIMGEN_EN=0 not refused");

    ctx.vs_mode = XLAT11_VS_UNKNOWN;
    uint32_t stages1[] = { 0xC0016900u, 0x2D5u, 1u << 13 };
    CHECK(xlat11_translate(&ctx, stages1, 3, g_out, CAP, &n, &st) == XLAT11_OK && ctx.vs_mode == XLAT11_VS_NGG,
          "NGG not learnt");

    /* SPI_SHADER_PGM_LO_VS 0xb120: legacy VS slot. */
    uint32_t lvs[] = { 0xC0017600u, 0x048u, 0x1234u };
    ctx.vs_mode = XLAT11_VS_UNKNOWN;
    CHECK(xlat11_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT11_ERR_VS_MODE_UNKNOWN,
          "legacy VS with unknown mode not refused");
    lvs[2] = 0;
    CHECK(xlat11_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 0, "zero legacy VS dropped");
    lvs[2] = 0x1234u; ctx.vs_mode = XLAT11_VS_NGG;
    CHECK(xlat11_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 0 &&
          st.regs_dropped_legacy_vs == 1, "legacy VS under NGG dropped");
}

/* ---- 8. index forms ------------------------------------------------------ */
static void test_index_form(void)
{
    xlat11_stats st; uint32_t n;
    /* SPI_SHADER_PGM_RSRC3_PS 0xb01c, dword offset 7, idx 3. CU_EN=0xF00F, WAVE_LIMIT=0. */
    uint32_t ix3[] = { 0xC0019B00u, 0x30000007u, 0x0000F00Fu };
    CHECK(xlat11_translate(0, ix3, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 3 &&
          g_out[0] == 0xC0019B00u && g_out[1] == 0x30000007u && g_out[2] == 0x0000F00Fu,
          "SET_SH_REG_INDEX RSRC3_PS: 0x%08x 0x%08x 0x%08x", n == 3 ? g_out[0] : 0, n == 3 ? g_out[1] : 0, n == 3 ? g_out[2] : 0);

    /* VGT_PRIMITIVE_TYPE 0x30908, uconfig offset 0x242, idx 1. */
    uint32_t ix1[] = { 0xC0017A00u, 0x10000242u, 0x3u };
    CHECK(xlat11_translate(0, ix1, 3, g_out, CAP, &n, &st) == XLAT11_OK && n == 3 &&
          g_out[0] == 0xC0017A00u && g_out[1] == 0x10000242u && g_out[2] == 0x3u,
          "SET_UCONFIG_REG_INDEX VGT_PRIMITIVE_TYPE");

    uint32_t ix0[] = { 0xC0019B00u, 0x00000007u, 1u };
    CHECK(xlat11_translate(0, ix0, 3, g_out, CAP, &n, &st) == XLAT11_ERR_BAD_INDEX, "SH index 0 accepted");
    uint32_t ixr[] = { 0xC0019B00u, 0x30010007u, 1u };
    CHECK(xlat11_translate(0, ixr, 3, g_out, CAP, &n, &st) == XLAT11_ERR_BAD_INDEX, "reserved offset bits accepted");
}

/* ---- 9. refusals --------------------------------------------------------- */
static void test_refusals(void)
{
    xlat11_stats st; uint32_t n;

    uint32_t mid[] = { 0xC0016900u, 0x3FFu, 9u };            /* 0x28ffc untabled */
    CHECK(xlat11_translate(0, mid, 3, g_out, CAP, &n, &st) == XLAT11_ERR_UNKNOWN_REG && st.err_reg_addr == 0x28FFCu &&
          st.err_name && strcmp(st.err_name, "UNTABLED_GFX10_ADDRESS") == 0, "untabled address not refused");

    uint32_t ml[][6] = { { 0xC0036100u, 0x1000u, 0, 0x10u, 4, 0 }, { 0xC0001200u, 0, 0, 0, 0, 0 },
                         { 0xC0012800u, 0x80000000u, 0x80000000u, 0, 0, 0 } };
    uint32_t mllen[] = { 5, 2, 3 };
    const char *mlname[] = { "LOAD_CONTEXT_REG", "CLEAR_STATE", "CONTEXT_CONTROL" };
    for (int k = 0; k < 3; k++) {
        xlat11_status s = xlat11_translate(0, ml[k], mllen[k], g_out, CAP, &n, &st);
        CHECK(s == XLAT11_ERR_MEMLOADED && st.err_name && strcmp(st.err_name, mlname[k]) == 0, "memloaded %s", mlname[k]);
    }

    uint32_t t0[] = { 0x00001234u };
    CHECK(xlat11_translate(0, t0, 1, g_out, CAP, &n, &st) == XLAT11_ERR_BAD_PACKET, "type-0 accepted");
    uint32_t tr[] = { 0xC0056900u, 0x1E0u, 1 };
    CHECK(xlat11_translate(0, tr, 3, g_out, CAP, &n, &st) == XLAT11_ERR_TRUNCATED, "truncated accepted");
    uint32_t rg[] = { 0xC0026900u, 0x1FFFu, 1, 2 };
    CHECK(xlat11_translate(0, rg, 4, g_out, CAP, &n, &st) == XLAT11_ERR_RANGE, "run past block end accepted");
    uint32_t wrap[] = { 0xC0016900u, 0xFFFFFFFFu, 1 };
    CHECK(xlat11_translate(0, wrap, 3, g_out, CAP, &n, &st) == XLAT11_ERR_RANGE, "wrapping offset accepted");
    CHECK(xlat11_translate(0, 0, 4, g_out, CAP, &n, &st) == XLAT11_ERR_ARG, "NULL input accepted");
}

/* ---- 10. capacity: never write past out_cap ------------------------------ */
static void test_capacity(void)
{
    const GoldenVec *g = &kGolden[0];   /* identical_run, exp_len 5 */
    uint32_t need = g->exp_len, bad = 0;
    for (uint32_t cap = 0; cap < need; cap++) {
        uint32_t *buf = (uint32_t *)malloc(((size_t)cap + 64) * sizeof(uint32_t));
        for (uint32_t k = 0; k < cap + 64; k++) buf[k] = 0xCAFEF00Du;
        xlat11_ctx ctx; ctx.vs_mode = XLAT11_VS_UNKNOWN;
        uint32_t n;
        xlat11_status s = xlat11_translate(&ctx, g->in, g->in_len, buf, cap, &n, 0);
        int canary = 1;
        for (uint32_t k = cap; k < cap + 64; k++) if (buf[k] != 0xCAFEF00Du) canary = 0;
        if (s != XLAT11_ERR_CAPACITY || !canary) bad++;
        free(buf);
    }
    CHECK(bad == 0, "capacity: %u of %u caps wrote past the end or did not report CAPACITY", bad, need);
    printf("  capacity: %s, every cap 0..%u -> ERR_CAPACITY, canary intact\n", g->name, need - 1);
}

int main(void)
{
    test_golden();
    test_table_classes();
    test_reused_priority();
    test_roundtrip();
    test_repack_hand_cases();
    test_split_flags_verbatim();
    test_ngg();
    test_index_form();
    test_refusals();
    test_capacity();
    printf("xlat11 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
