/*
 * test_dcn314.c - host unit tests for the dcn314 device/address layer and the read-only OTG/HUBP/DIO/PWM
 * readouts. Does not need Linux or hardware.
 *
 * The expected absolute addresses are written by hand from Linux v7.2 dcn_3_1_4_offset.h (for example
 * regOTG0_OTG_CONTROL = 0x1b41 with BASE_IDX 2, so absolute = 0x10000 + 0x1b41 = 0x11b41 under the
 * placeholder seg2), so a generator or addressing mistake cannot pass by agreeing with itself.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn314.h"
#include "dcn314_io.h"
#include "dcn314_ro.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MMIO_DWORDS 0x40000u
static const uint32_t SEG[5] = {
    0x00000000u, 0x00001000u, 0x00010000u, 0x00020000u, 0x00030000u
};

struct model {
    uint32_t mem[MMIO_DWORDS];
    int reads, writes;
};

static uint32_t m_read(void *c, uint32_t a)
{
    struct model *m = c;
    m->reads++;
    return a < MMIO_DWORDS ? m->mem[a] : 0xdeadbeefu;
}

static void m_write(void *c, uint32_t a, uint32_t v)
{
    struct model *m = c;
    m->writes++;
    if (a < MMIO_DWORDS)
        m->mem[a] = v;
}

static void m_udelay(void *c, uint32_t us)
{
    (void)c;
    (void)us;
}

#define FIELD(mask, shift, v) (((uint32_t)(v) << (shift)) & (mask))
#define PUT(m, off, base, v) ((m)->mem[((base) == 2u ? SEG[2] : (base)) + (off)] = (v))

static struct model *new_model(void)
{
    return calloc(1, sizeof(struct model));
}

static int init_dev(struct dcn314_dev *d, struct model *m)
{
    return dcn314_dev_init(d, m, m_read, m_write, m_udelay, NULL, SEG, MMIO_DWORDS, 0);
}

static void test_init_abs(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    uint32_t bad[5];

    /* Hand-checked Linux v7.2 offsets and BASE_IDX. */
    CHECK(DCN314_OTG_OTG_CONTROL(0) == 0x1b41u, "OTG_CONTROL 0x%x", DCN314_OTG_OTG_CONTROL(0));
    CHECK(DCN314_OTG_OTG_CONTROL_BASE_IDX == 2u, "OTG_CONTROL base %u", DCN314_OTG_OTG_CONTROL_BASE_IDX);
    CHECK(DCN314_OTG_OTG_H_TOTAL(0) == 0x1b2au, "OTG_H_TOTAL 0x%x", DCN314_OTG_OTG_H_TOTAL(0));
    CHECK(DCN314_HUBP_DCHUBP_CNTL(0) == 0x05f3u, "DCHUBP_CNTL 0x%x", DCN314_HUBP_DCHUBP_CNTL(0));
    CHECK(DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(0) == 0x060au, "PRIMARY_SURFACE_ADDRESS 0x%x",
          DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(0));
    CHECK(DCN314_AUX_AUX_SW_DATA(0) == 0x1f56u, "AUX_SW_DATA 0x%x", DCN314_AUX_AUX_SW_DATA(0));
    CHECK(DCN314_PWRSEQ_BL_PWM_CNTL(0) == 0x2f19u, "BL_PWM_CNTL 0x%x", DCN314_PWRSEQ_BL_PWM_CNTL(0));

    CHECK(dcn314_dev_init(&d, m, m_read, m_write, m_udelay, NULL, SEG, MMIO_DWORDS, 0) == DCN314_OK, "init ok");
    CHECK(dcn314_abs(&d, 0x1b41u, 2u) == 0x11b41u, "abs OTG_CONTROL 0x%x", dcn314_abs(&d, 0x1b41u, 2u));
    CHECK(dcn314_abs(&d, 0x1b41u, 0u) == DCN314_BAD_ADDR, "base 0 unused");
    CHECK(dcn314_abs(&d, 0x1b41u, 5u) == DCN314_BAD_ADDR, "base 5 out of range");
    CHECK(dcn314_abs(&d, DCN314_BAD_OFFSET, 2u) == DCN314_BAD_ADDR, "bad offset");
    {
        struct dcn314_dev u;
        memset(&u, 0, sizeof(u));
        CHECK(dcn314_abs(&u, 0x1b41u, 2u) == DCN314_BAD_ADDR, "uninit dev");
    }

    CHECK(dcn314_dev_init(&d, m, NULL, m_write, m_udelay, NULL, SEG, MMIO_DWORDS, 0) == DCN314_E_ARG, "null rreg");
    CHECK(dcn314_dev_init(&d, m, m_read, NULL, m_udelay, NULL, SEG, MMIO_DWORDS, 0) == DCN314_E_ARG, "null wreg");
    CHECK(dcn314_dev_init(&d, m, m_read, m_write, NULL, NULL, SEG, MMIO_DWORDS, 0) == DCN314_E_ARG, "null udelay");
    CHECK(dcn314_dev_init(&d, m, m_read, m_write, m_udelay, NULL, NULL, MMIO_DWORDS, 0) == DCN314_E_ARG, "null seg");
    CHECK(dcn314_dev_init(&d, m, m_read, m_write, m_udelay, NULL, SEG, 0u, 0) == DCN314_E_ARG, "zero window");

    memcpy(bad, SEG, sizeof(bad));
    bad[2] = 0u;
    CHECK(dcn314_dev_init(&d, m, m_read, m_write, m_udelay, NULL, bad, MMIO_DWORDS, 0) == DCN314_E_WINDOW,
          "zero used seg");
    CHECK(dcn314_dev_init(&d, m, m_read, m_write, m_udelay, NULL, SEG, 0x10000u, 0) == DCN314_E_WINDOW,
          "seg2 + max offset past window");

    free(m);
}

static void test_otg(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    struct dcn314_otg_timing t;
    struct dcn314_otg_position p;
    bool vstartup = false, vupdate = false, master = false;
    uint32_t fc = 0u;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    PUT(m, DCN314_OTG_OTG_H_TOTAL(0), 0x10000,
        FIELD(DCN314_OTG_OTG_H_TOTAL__OTG_H_TOTAL_MASK,
              DCN314_OTG_OTG_H_TOTAL__OTG_H_TOTAL__SHIFT, 0x1234u));
    PUT(m, DCN314_OTG_OTG_V_TOTAL(0), 0x10000,
        FIELD(DCN314_OTG_OTG_V_TOTAL__OTG_V_TOTAL_MASK,
              DCN314_OTG_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT, 0x0567u));
    PUT(m, DCN314_OTG_OTG_H_BLANK_START_END(0), 0x10000,
        FIELD(DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_START_MASK,
              DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_START__SHIFT, 0x0100u) |
        FIELD(DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_END_MASK,
              DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT, 0x01ffu));
    PUT(m, DCN314_OTG_OTG_V_BLANK_START_END(0), 0x10000,
        FIELD(DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK,
              DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT, 0x0010u) |
        FIELD(DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK,
              DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT, 0x001fu));

    CHECK(dcn314_otg_read_timing(&d, 0, &t) == DCN314_OK, "timing ok");
    CHECK(t.h_total == 0x1234u, "h_total 0x%x", t.h_total);
    CHECK(t.v_total == 0x0567u, "v_total 0x%x", t.v_total);
    CHECK(t.h_blank_start == 0x0100u, "h_blank_start 0x%x", t.h_blank_start);
    CHECK(t.h_blank_end == 0x01ffu, "h_blank_end 0x%x", t.h_blank_end);
    CHECK(t.v_blank_start == 0x0010u, "v_blank_start 0x%x", t.v_blank_start);
    CHECK(t.v_blank_end == 0x001fu, "v_blank_end 0x%x", t.v_blank_end);
    CHECK(dcn314_otg_read_timing(&d, 4, &t) == DCN314_E_INST, "otg 4 inst");
    CHECK(dcn314_otg_read_timing(NULL, 0, &t) == DCN314_E_ARG, "otg null");

    PUT(m, DCN314_OTG_OTG_STATUS_POSITION(0), 0x10000,
        FIELD(DCN314_OTG_OTG_STATUS_POSITION__OTG_VERT_COUNT_MASK,
              DCN314_OTG_OTG_STATUS_POSITION__OTG_VERT_COUNT__SHIFT, 0x0033u) |
        FIELD(DCN314_OTG_OTG_STATUS_POSITION__OTG_HORZ_COUNT_MASK,
              DCN314_OTG_OTG_STATUS_POSITION__OTG_HORZ_COUNT__SHIFT, 0x0444u));
    PUT(m, DCN314_OTG_OTG_NOM_VERT_POSITION(0), 0x10000,
        FIELD(DCN314_OTG_OTG_NOM_VERT_POSITION__OTG_VERT_COUNT_NOM_MASK,
              DCN314_OTG_OTG_NOM_VERT_POSITION__OTG_VERT_COUNT_NOM__SHIFT, 0x0055u));
    CHECK(dcn314_otg_read_position(&d, 0, &p) == DCN314_OK, "pos ok");
    CHECK(p.vert_count == 0x33u && p.horz_count == 0x444u && p.vert_count_nom == 0x55u,
          "pos %u/%u/%u", p.vert_count, p.horz_count, p.vert_count_nom);

    PUT(m, DCN314_OTG_OTG_STATUS_FRAME_COUNT(0), 0x10000,
        FIELD(DCN314_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK,
              DCN314_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT__SHIFT, 0x00abcdefu));
    CHECK(dcn314_otg_read_frame_count(&d, 0, &fc) == DCN314_OK && fc == 0xabcdefu, "frame %x", fc);

    PUT(m, DCN314_OTG_OTG_GLOBAL_SYNC_STATUS(0), 0x10000,
        DCN314_OTG_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK |
        DCN314_OTG_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK);
    CHECK(dcn314_otg_read_sync_status(&d, 0, &vstartup, &vupdate) == DCN314_OK,
          "sync ok");
    CHECK(vstartup && vupdate, "sync pending");

    PUT(m, DCN314_OTG_OTG_CONTROL(0), 0x10000, DCN314_OTG_OTG_CONTROL__OTG_MASTER_EN_MASK);
    CHECK(dcn314_otg_is_master_enabled(&d, 0, &master) == DCN314_OK && master, "master enabled");

    free(m);
}

static void test_hubp(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    struct dcn314_hubp_surface s;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    PUT(m, DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(1),
        DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX, 0x89abcdefu);
    PUT(m, DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH(1),
        DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX,
        FIELD(DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK,
              DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH__SHIFT,
              0x1234u));
    PUT(m, DCN314_HUBP_DCSURF_SURFACE_CONFIG(1), DCN314_HUBP_DCSURF_SURFACE_CONFIG_BASE_IDX,
        FIELD(DCN314_HUBP_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK,
              DCN314_HUBP_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT, 4u));
    PUT(m, DCN314_HUBP_DCSURF_TILING_CONFIG(1), DCN314_HUBP_DCSURF_TILING_CONFIG_BASE_IDX,
        FIELD(DCN314_HUBP_DCSURF_TILING_CONFIG__SW_MODE_MASK,
              DCN314_HUBP_DCSURF_TILING_CONFIG__SW_MODE__SHIFT, 5u) |
        DCN314_HUBP_DCSURF_TILING_CONFIG__META_LINEAR_MASK |
        FIELD(DCN314_HUBP_DCSURF_TILING_CONFIG__PIPE_ALIGNED_MASK,
              DCN314_HUBP_DCSURF_TILING_CONFIG__PIPE_ALIGNED__SHIFT, 0u));
    PUT(m, DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL(1), DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL_BASE_IDX,
        DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK);
    PUT(m, DCN314_HUBP_DCHUBP_CNTL(1), DCN314_HUBP_DCHUBP_CNTL_BASE_IDX,
        DCN314_HUBP_DCHUBP_CNTL__HUBP_IN_BLANK_MASK);
    PUT(m, DCN314_HUBP_DCSURF_PRI_VIEWPORT_START(1), DCN314_HUBP_DCSURF_PRI_VIEWPORT_START_BASE_IDX,
        FIELD(DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START_MASK,
              DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START__SHIFT, 10u) |
        FIELD(DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START_MASK,
              DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START__SHIFT, 20u));
    PUT(m, DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION(1),
        DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION_BASE_IDX,
        FIELD(DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK,
              DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH__SHIFT, 1920u) |
        FIELD(DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK,
              DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT__SHIFT, 1080u));

    CHECK(dcn314_hubp_read_surface(&d, 1, &s) == DCN314_OK, "hubp ok");
    CHECK(s.primary_address == 0x123489abcdefull, "addr %" PRIx64, s.primary_address);
    CHECK(s.pixel_format == 4u && s.sw_mode == 5u && s.meta_linear == 1u && s.pipe_aligned == 0u,
          "format %u sw %u meta %u pa %u", s.pixel_format, s.sw_mode, s.meta_linear, s.pipe_aligned);
    CHECK(s.dcc_en == 1u && s.in_blank == 1u, "dcc %u blank %u", s.dcc_en, s.in_blank);
    CHECK(s.viewport_x == 10u && s.viewport_y == 20u && s.viewport_w == 1920u && s.viewport_h == 1080u,
          "vp %u,%u %ux%u", s.viewport_x, s.viewport_y, s.viewport_w, s.viewport_h);
    CHECK(dcn314_hubp_read_surface(&d, 4, &s) == DCN314_E_INST, "hubp 4 inst");

    free(m);
}

static void test_dio(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    uint8_t et = 0, dp_sel = 0, hdmi_sel = 0;
    bool up = false, embedded = false, fec = false;
    bool fe = false, be = false, fe_clk = false, be_clk = false;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    PUT(m, DCN314_DIO_LINKA_CNTL, DCN314_DIO_LINKA_CNTL_BASE_IDX,
        FIELD(DCN314_DIO_LINKA_CNTL__ENC_TYPE_SEL_MASK,
              DCN314_DIO_LINKA_CNTL__ENC_TYPE_SEL__SHIFT, 1u) |
        FIELD(DCN314_DIO_LINKA_CNTL__HPO_DP_ENC_SEL_MASK,
              DCN314_DIO_LINKA_CNTL__HPO_DP_ENC_SEL__SHIFT, 2u) |
        FIELD(DCN314_DIO_LINKA_CNTL__HPO_HDMI_ENC_SEL_MASK,
              DCN314_DIO_LINKA_CNTL__HPO_HDMI_ENC_SEL__SHIFT, 3u));
    CHECK(dcn314_dio_link_read_type(&d, 0, &et, &dp_sel, &hdmi_sel) == DCN314_OK, "dio ok");
    CHECK(et == 1u && dp_sel == 2u && hdmi_sel == 3u, "dio %u/%u/%u", et, dp_sel, hdmi_sel);
    CHECK(dcn314_dio_link_read_type(&d, 6, &et, &dp_sel, &hdmi_sel) == DCN314_E_INST, "dio 6 inst");

    PUT(m, DCN314_DP_DP_LINK_CNTL(0), DCN314_DP_DP_LINK_CNTL_BASE_IDX,
        DCN314_DP_DP_LINK_CNTL__DP_LINK_STATUS_MASK |
        DCN314_DP_DP_LINK_CNTL__DP_EMBEDDED_PANEL_MODE_MASK);
    PUT(m, DCN314_DP_DP_DPHY_CNTL(0), DCN314_DP_DP_DPHY_CNTL_BASE_IDX,
        DCN314_DP_DP_DPHY_CNTL__DPHY_FEC_ACTIVE_STATUS_MASK);
    CHECK(dcn314_dp_read_link_status(&d, 0, &up, &embedded, &fec) == DCN314_OK, "dp ok");
    CHECK(up && embedded && fec, "dp status");

    /* DIG_SOURCE_SELECT = 0 means OTG0 (not "disabled"); only DIG_SYMCLK_FE_ON carries FE state. */
    PUT(m, DCN314_DIG_DIG_FE_CNTL(0), DCN314_DIG_DIG_FE_CNTL_BASE_IDX,
        FIELD(DCN314_DIG_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK,
              DCN314_DIG_DIG_FE_CNTL__DIG_SOURCE_SELECT__SHIFT, 0u) |
        DCN314_DIG_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK);
    PUT(m, DCN314_DIG_DIG_BE_EN_CNTL(0), DCN314_DIG_DIG_BE_EN_CNTL_BASE_IDX,
        DCN314_DIG_DIG_BE_EN_CNTL__DIG_ENABLE_MASK);
    CHECK(dcn314_dig_read_status(&d, 0, &fe, &be, &fe_clk, &be_clk) == DCN314_OK, "dig ok");
    CHECK(fe && be && fe_clk && !be_clk, "dig status %d%d%d%d", fe, be, fe_clk, be_clk);

    free(m);
}

static void test_pwm(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    struct dcn314_pwm_state p;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    PUT(m, DCN314_PWRSEQ_BL_PWM_CNTL(0), DCN314_PWRSEQ_BL_PWM_CNTL_BASE_IDX,
        DCN314_PWRSEQ_BL_PWM_CNTL__BL_PWM_EN_MASK |
        FIELD(DCN314_PWRSEQ_BL_PWM_CNTL__BL_ACTIVE_INT_FRAC_CNT_MASK,
              DCN314_PWRSEQ_BL_PWM_CNTL__BL_ACTIVE_INT_FRAC_CNT__SHIFT, 0x8000u));
    PUT(m, DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL(0), DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL_BASE_IDX,
        FIELD(DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_MASK,
              DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD__SHIFT, 0x3ffu) |
        FIELD(DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_BITCNT_MASK,
              DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_BITCNT__SHIFT, 10u));
    PUT(m, DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1(0), DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1_BASE_IDX,
        FIELD(DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1__BL_PWM_REF_DIV_MASK,
              DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1__BL_PWM_REF_DIV__SHIFT, 0x1234u));
    PUT(m, DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2(0), DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2_BASE_IDX,
        FIELD(DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__XTAL_REF_DIV_MASK,
              DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__XTAL_REF_DIV__SHIFT, 0x12u) |
        FIELD(DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__MICROSECOND_TIME_BASE_DIV_MASK,
              DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__MICROSECOND_TIME_BASE_DIV__SHIFT, 0x34u));

    CHECK(dcn314_pwm_read_state(&d, 0, &p) == DCN314_OK, "pwm ok");
    CHECK(p.enabled && !p.fractional, "pwm en %d frac %d", p.enabled, p.fractional);
    CHECK(p.active_int_frac == 0x8000u && p.period == 0x3ffu && p.period_bitcnt == 10u,
          "pwm raw %x %x %u", p.active_int_frac, p.period, p.period_bitcnt);
    CHECK(p.ref_div == 0x1234u && p.xtal_ref_div == 0x12u && p.us_timebase_div == 0x34u,
          "pwm div %x %x %x", p.ref_div, p.xtal_ref_div, p.us_timebase_div);
    /* Linux dce_get_16_bit_backlight_from_pwm(): 0x8000 << 11 = 0x4000000, / 0x3ff = 65600,
     * (65600 + 1) >> 1 = 32800. */
    CHECK(p.backlight_level == 32800u, "backlight %u", p.backlight_level);
    CHECK(dcn314_pwm_read_state(&d, 2, &p) == DCN314_E_INST, "pwm 2 inst");

    free(m);
}

static void test_write_refused(void)
{
    struct dcn314_dev d;
    struct model *m = new_model();
    int before;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    before = m->writes;
    CHECK(dcn314_write(&d, 0x1b41u, 2u, 0xdeadbeefu) == DCN314_E_NOWRITE, "write stub");
    CHECK(dcn314_update(&d, 0x1b41u, 2u, 0xffffu, 0, 1u) == DCN314_E_NOWRITE, "update stub");
    CHECK(dcn314_set(&d, 0x1b41u, 2u, 0xffffu, 1u) == DCN314_E_NOWRITE, "set stub");
    CHECK(dcn314_stage1_write_refused() == DCN314_E_NOWRITE, "refused stub");
    CHECK(m->writes == before, "stubs touched %d writes", m->writes - before);

    free(m);
}

int main(void)
{
    test_init_abs();
    test_otg();
    test_hubp();
    test_dio();
    test_pwm();
    test_write_refused();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
