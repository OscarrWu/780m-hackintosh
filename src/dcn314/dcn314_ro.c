/*
 * dcn314_ro.c - DCN 3.1.4 read-only readout functions. See dcn314_ro.h.
 * Pure reads only; never writes, never locks. Survey (D6/D7/D8) dependencies.
 */
#include "dcn314_ro.h"
#include "dcn314_io.h"

#pragma GCC poison float double

static inline int check_inst(unsigned inst, unsigned max)
{
    return inst < max ? DCN314_OK : DCN314_E_INST;
}

/* ---------- OTG ---------- */

int dcn314_otg_read_timing(const struct dcn314_dev *dev, unsigned otg, struct dcn314_otg_timing *out)
{
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(otg, DCN314_NUM_OTG) != DCN314_OK)
        return DCN314_E_INST;
    out->h_total = dcn314_get(dev, DCN314_OTG_OTG_H_TOTAL(otg), DCN314_OTG_OTG_H_TOTAL_BASE_IDX,
                             DCN314_OTG_OTG_H_TOTAL__OTG_H_TOTAL_MASK,
                             DCN314_OTG_OTG_H_TOTAL__OTG_H_TOTAL__SHIFT);
    out->v_total = dcn314_get(dev, DCN314_OTG_OTG_V_TOTAL(otg), DCN314_OTG_OTG_V_TOTAL_BASE_IDX,
                             DCN314_OTG_OTG_V_TOTAL__OTG_V_TOTAL_MASK,
                             DCN314_OTG_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT);
    out->h_blank_start = dcn314_get(dev, DCN314_OTG_OTG_H_BLANK_START_END(otg),
                                    DCN314_OTG_OTG_H_BLANK_START_END_BASE_IDX,
                                    DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_START_MASK,
                                    DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_START__SHIFT);
    out->h_blank_end = dcn314_get(dev, DCN314_OTG_OTG_H_BLANK_START_END(otg),
                                  DCN314_OTG_OTG_H_BLANK_START_END_BASE_IDX,
                                  DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_END_MASK,
                                  DCN314_OTG_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT);
    out->v_blank_start = dcn314_get(dev, DCN314_OTG_OTG_V_BLANK_START_END(otg),
                                    DCN314_OTG_OTG_V_BLANK_START_END_BASE_IDX,
                                    DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK,
                                    DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT);
    out->v_blank_end = dcn314_get(dev, DCN314_OTG_OTG_V_BLANK_START_END(otg),
                                  DCN314_OTG_OTG_V_BLANK_START_END_BASE_IDX,
                                  DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK,
                                  DCN314_OTG_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT);
    return DCN314_OK;
}

int dcn314_otg_read_position(const struct dcn314_dev *dev, unsigned otg, struct dcn314_otg_position *out)
{
    uint32_t raw;
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(otg, DCN314_NUM_OTG) != DCN314_OK)
        return DCN314_E_INST;
    raw = dcn314_read(dev, DCN314_OTG_OTG_STATUS_POSITION(otg), DCN314_OTG_OTG_STATUS_POSITION_BASE_IDX);
    out->vert_count = (raw & DCN314_OTG_OTG_STATUS_POSITION__OTG_VERT_COUNT_MASK) >>
                     DCN314_OTG_OTG_STATUS_POSITION__OTG_VERT_COUNT__SHIFT;
    out->horz_count = (raw & DCN314_OTG_OTG_STATUS_POSITION__OTG_HORZ_COUNT_MASK) >>
                     DCN314_OTG_OTG_STATUS_POSITION__OTG_HORZ_COUNT__SHIFT;
    out->vert_count_nom = dcn314_get(dev, DCN314_OTG_OTG_NOM_VERT_POSITION(otg),
                                     DCN314_OTG_OTG_NOM_VERT_POSITION_BASE_IDX,
                                     DCN314_OTG_OTG_NOM_VERT_POSITION__OTG_VERT_COUNT_NOM_MASK,
                                     DCN314_OTG_OTG_NOM_VERT_POSITION__OTG_VERT_COUNT_NOM__SHIFT);
    return DCN314_OK;
}

int dcn314_otg_read_frame_count(const struct dcn314_dev *dev, unsigned otg, uint32_t *out)
{
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(otg, DCN314_NUM_OTG) != DCN314_OK)
        return DCN314_E_INST;
    *out = dcn314_get(dev, DCN314_OTG_OTG_STATUS_FRAME_COUNT(otg), DCN314_OTG_OTG_STATUS_FRAME_COUNT_BASE_IDX,
                     DCN314_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK,
                     DCN314_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT__SHIFT);
    return DCN314_OK;
}

int dcn314_otg_read_sync_status(const struct dcn314_dev *dev, unsigned otg,
                                bool *vstartup_pending, bool *vupdate_no_lock_pending)
{
    uint32_t raw;
    if (!dev)
        return DCN314_E_ARG;
    if (check_inst(otg, DCN314_NUM_OTG) != DCN314_OK)
        return DCN314_E_INST;
    raw = dcn314_read(dev, DCN314_OTG_OTG_GLOBAL_SYNC_STATUS(otg), DCN314_OTG_OTG_GLOBAL_SYNC_STATUS_BASE_IDX);
    if (vstartup_pending)
        *vstartup_pending = (raw & DCN314_OTG_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK) != 0u;
    if (vupdate_no_lock_pending)
        *vupdate_no_lock_pending =
            (raw & DCN314_OTG_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK) != 0u;
    return DCN314_OK;
}

int dcn314_otg_is_master_enabled(const struct dcn314_dev *dev, unsigned otg, bool *out)
{
    uint32_t raw;
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(otg, DCN314_NUM_OTG) != DCN314_OK)
        return DCN314_E_INST;
    raw = dcn314_read(dev, DCN314_OTG_OTG_CONTROL(otg), DCN314_OTG_OTG_CONTROL_BASE_IDX);
    *out = (raw & DCN314_OTG_OTG_CONTROL__OTG_MASTER_EN_MASK) != 0u;
    return DCN314_OK;
}

/* ---------- HUBP ---------- */

int dcn314_hubp_read_surface(const struct dcn314_dev *dev, unsigned hubp,
                             struct dcn314_hubp_surface *out)
{
    uint32_t lo, hi, cfg, tile, sc, vp_start, vp_dim, hubp_ctl;
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(hubp, DCN314_NUM_HUBP) != DCN314_OK)
        return DCN314_E_INST;

    lo = dcn314_read(dev, DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(hubp),
                     DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX);
    hi = dcn314_get(dev, DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH(hubp),
                    DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX,
                    DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK,
                    DCN314_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH__SHIFT);
    out->primary_address = ((uint64_t)hi << 32) | lo;

    cfg = dcn314_read(dev, DCN314_HUBP_DCSURF_SURFACE_CONFIG(hubp), DCN314_HUBP_DCSURF_SURFACE_CONFIG_BASE_IDX);
    out->pixel_format = (uint8_t)((cfg & DCN314_HUBP_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) >>
                                  DCN314_HUBP_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT);

    tile = dcn314_read(dev, DCN314_HUBP_DCSURF_TILING_CONFIG(hubp), DCN314_HUBP_DCSURF_TILING_CONFIG_BASE_IDX);
    out->sw_mode = (uint8_t)((tile & DCN314_HUBP_DCSURF_TILING_CONFIG__SW_MODE_MASK) >>
                             DCN314_HUBP_DCSURF_TILING_CONFIG__SW_MODE__SHIFT);
    out->meta_linear = (uint8_t)((tile & DCN314_HUBP_DCSURF_TILING_CONFIG__META_LINEAR_MASK) >>
                                 DCN314_HUBP_DCSURF_TILING_CONFIG__META_LINEAR__SHIFT);
    out->pipe_aligned = (uint8_t)((tile & DCN314_HUBP_DCSURF_TILING_CONFIG__PIPE_ALIGNED_MASK) >>
                                  DCN314_HUBP_DCSURF_TILING_CONFIG__PIPE_ALIGNED__SHIFT);

    sc = dcn314_read(dev, DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL(hubp),
                     DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL_BASE_IDX);
    out->dcc_en = (uint8_t)((sc & DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK) >>
                            DCN314_HUBPREQ_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN__SHIFT);

    hubp_ctl = dcn314_read(dev, DCN314_HUBP_DCHUBP_CNTL(hubp), DCN314_HUBP_DCHUBP_CNTL_BASE_IDX);
    out->in_blank = (uint8_t)((hubp_ctl & DCN314_HUBP_DCHUBP_CNTL__HUBP_IN_BLANK_MASK) >>
                              DCN314_HUBP_DCHUBP_CNTL__HUBP_IN_BLANK__SHIFT);

    vp_start = dcn314_read(dev, DCN314_HUBP_DCSURF_PRI_VIEWPORT_START(hubp),
                           DCN314_HUBP_DCSURF_PRI_VIEWPORT_START_BASE_IDX);
    out->viewport_x = (uint16_t)((vp_start & DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START_MASK) >>
                                 DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START__SHIFT);
    out->viewport_y = (uint16_t)((vp_start & DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START_MASK) >>
                                 DCN314_HUBP_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START__SHIFT);

    vp_dim = dcn314_read(dev, DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION(hubp),
                         DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION_BASE_IDX);
    out->viewport_w = (uint16_t)((vp_dim & DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK) >>
                                 DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH__SHIFT);
    out->viewport_h = (uint16_t)((vp_dim & DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK) >>
                                 DCN314_HUBP_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT__SHIFT);
    return DCN314_OK;
}

/* ---------- DIO / DP / DIG ---------- */

int dcn314_dio_link_read_type(const struct dcn314_dev *dev, unsigned link,
                              uint8_t *enc_type_sel, uint8_t *hpo_dp_enc_sel, uint8_t *hpo_hdmi_enc_sel)
{
    static const uint32_t cntl[DCN314_NUM_DIO_LINK] = {
        DCN314_DIO_LINKA_CNTL, DCN314_DIO_LINKB_CNTL, DCN314_DIO_LINKC_CNTL,
        DCN314_DIO_LINKD_CNTL, DCN314_DIO_LINKE_CNTL, DCN314_DIO_LINKF_CNTL,
    };
    static const uint32_t bidx[DCN314_NUM_DIO_LINK] = {
        DCN314_DIO_LINKA_CNTL_BASE_IDX, DCN314_DIO_LINKB_CNTL_BASE_IDX, DCN314_DIO_LINKC_CNTL_BASE_IDX,
        DCN314_DIO_LINKD_CNTL_BASE_IDX, DCN314_DIO_LINKE_CNTL_BASE_IDX, DCN314_DIO_LINKF_CNTL_BASE_IDX,
    };
    static const uint32_t etype[DCN314_NUM_DIO_LINK] = {
        DCN314_DIO_LINKA_CNTL__ENC_TYPE_SEL_MASK, DCN314_DIO_LINKB_CNTL__ENC_TYPE_SEL_MASK,
        DCN314_DIO_LINKC_CNTL__ENC_TYPE_SEL_MASK, DCN314_DIO_LINKD_CNTL__ENC_TYPE_SEL_MASK,
        DCN314_DIO_LINKE_CNTL__ENC_TYPE_SEL_MASK, DCN314_DIO_LINKF_CNTL__ENC_TYPE_SEL_MASK,
    };
    static const uint32_t etype_s[DCN314_NUM_DIO_LINK] = {
        DCN314_DIO_LINKA_CNTL__ENC_TYPE_SEL__SHIFT, DCN314_DIO_LINKB_CNTL__ENC_TYPE_SEL__SHIFT,
        DCN314_DIO_LINKC_CNTL__ENC_TYPE_SEL__SHIFT, DCN314_DIO_LINKD_CNTL__ENC_TYPE_SEL__SHIFT,
        DCN314_DIO_LINKE_CNTL__ENC_TYPE_SEL__SHIFT, DCN314_DIO_LINKF_CNTL__ENC_TYPE_SEL__SHIFT,
    };
    uint32_t raw;
    if (!dev)
        return DCN314_E_ARG;
    if (check_inst(link, DCN314_NUM_DIO_LINK) != DCN314_OK)
        return DCN314_E_INST;
    raw = dcn314_read(dev, cntl[link], bidx[link]);
    if (enc_type_sel)
        *enc_type_sel = (uint8_t)((raw & etype[link]) >> etype_s[link]);
    if (hpo_dp_enc_sel)
        *hpo_dp_enc_sel = (uint8_t)((raw & DCN314_DIO_LINKA_CNTL__HPO_DP_ENC_SEL_MASK) >>
                                    DCN314_DIO_LINKA_CNTL__HPO_DP_ENC_SEL__SHIFT);
    if (hpo_hdmi_enc_sel)
        *hpo_hdmi_enc_sel = (uint8_t)((raw & DCN314_DIO_LINKA_CNTL__HPO_HDMI_ENC_SEL_MASK) >>
                                      DCN314_DIO_LINKA_CNTL__HPO_HDMI_ENC_SEL__SHIFT);
    return DCN314_OK;
}

int dcn314_dp_read_link_status(const struct dcn314_dev *dev, unsigned dp,
                               bool *link_up, bool *embedded_panel, bool *fec_active)
{
    uint32_t lc, dphy;
    if (!dev)
        return DCN314_E_ARG;
    if (check_inst(dp, DCN314_NUM_DP) != DCN314_OK)
        return DCN314_E_INST;
    lc = dcn314_read(dev, DCN314_DP_DP_LINK_CNTL(dp), DCN314_DP_DP_LINK_CNTL_BASE_IDX);
    if (link_up)
        *link_up = (lc & DCN314_DP_DP_LINK_CNTL__DP_LINK_STATUS_MASK) != 0u;
    if (embedded_panel)
        *embedded_panel = (lc & DCN314_DP_DP_LINK_CNTL__DP_EMBEDDED_PANEL_MODE_MASK) != 0u;
    dphy = dcn314_read(dev, DCN314_DP_DP_DPHY_CNTL(dp), DCN314_DP_DP_DPHY_CNTL_BASE_IDX);
    if (fec_active)
        *fec_active = (dphy & DCN314_DP_DP_DPHY_CNTL__DPHY_FEC_ACTIVE_STATUS_MASK) != 0u;
    return DCN314_OK;
}

int dcn314_dig_read_status(const struct dcn314_dev *dev, unsigned dig,
                           bool *fe_enabled, bool *be_enabled, bool *fe_symclk, bool *be_symclk)
{
    uint32_t fe, be;
    if (!dev)
        return DCN314_E_ARG;
    if (check_inst(dig, DCN314_NUM_DIG) != DCN314_OK)
        return DCN314_E_INST;
    fe = dcn314_read(dev, DCN314_DIG_DIG_FE_CNTL(dig), DCN314_DIG_DIG_FE_CNTL_BASE_IDX);
    be = dcn314_read(dev, DCN314_DIG_DIG_BE_EN_CNTL(dig), DCN314_DIG_DIG_BE_EN_CNTL_BASE_IDX);
    /* DCN 3.1.4 has no dedicated DIG front-end enable bit. DIG_SOURCE_SELECT[2:0]
     * (DIGn_DIG_FE_CNTL, dcn_3_1_4_sh_mask.h) is the OTG instance, where 0 means OTG0,
     * so "!= 0" is not "enabled". DIG_SYMCLK_FE_ON (bit 24) is the FE running status
     * Linux itself reads (dcn314_dio_stream_encoder.c: enc314_reset_fifo). */
    if (fe_enabled)
        *fe_enabled = (fe & DCN314_DIG_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK) != 0u;
    if (fe_symclk)
        *fe_symclk = (fe & DCN314_DIG_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK) != 0u;
    if (be_enabled)
        *be_enabled = (be & DCN314_DIG_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) != 0u;
    if (be_symclk)
        *be_symclk = (be & DCN314_DIG_DIG_BE_EN_CNTL__DIG_SYMCLK_BE_ON_MASK) != 0u;
    return DCN314_OK;
}

/* ---------- DIO PWM (backlight) ---------- */

int dcn314_pwm_read_state(const struct dcn314_dev *dev, unsigned pwrseq, struct dcn314_pwm_state *out)
{
    uint32_t cntl, period, rd1, rd2;
    uint32_t bitcnt, period_mask, pwm_mask, bl_pwm, bl_period;
    uint64_t level;
    if (!dev || !out)
        return DCN314_E_ARG;
    if (check_inst(pwrseq, DCN314_NUM_PWRSEQ) != DCN314_OK)
        return DCN314_E_INST;

    cntl = dcn314_read(dev, DCN314_PWRSEQ_BL_PWM_CNTL(pwrseq), DCN314_PWRSEQ_BL_PWM_CNTL_BASE_IDX);
    out->enabled = (cntl & DCN314_PWRSEQ_BL_PWM_CNTL__BL_PWM_EN_MASK) != 0u;
    out->fractional = (cntl & DCN314_PWRSEQ_BL_PWM_CNTL__BL_PWM_FRACTIONAL_EN_MASK) != 0u;
    out->active_int_frac = (cntl & DCN314_PWRSEQ_BL_PWM_CNTL__BL_ACTIVE_INT_FRAC_CNT_MASK) >>
                           DCN314_PWRSEQ_BL_PWM_CNTL__BL_ACTIVE_INT_FRAC_CNT__SHIFT;

    period = dcn314_read(dev, DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL(pwrseq), DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL_BASE_IDX);
    out->period = (period & DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_MASK) >>
                  DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD__SHIFT;
    out->period_bitcnt = (period & DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_BITCNT_MASK) >>
                         DCN314_PWRSEQ_BL_PWM_PERIOD_CNTL__BL_PWM_PERIOD_BITCNT__SHIFT;

    rd1 = dcn314_read(dev, DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1(pwrseq), DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1_BASE_IDX);
    out->ref_div = (uint16_t)((rd1 & DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1__BL_PWM_REF_DIV_MASK) >>
                              DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV1__BL_PWM_REF_DIV__SHIFT);
    rd2 = dcn314_read(dev, DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2(pwrseq), DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2_BASE_IDX);
    out->xtal_ref_div = (uint8_t)((rd2 & DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__XTAL_REF_DIV_MASK) >>
                                  DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__XTAL_REF_DIV__SHIFT);
    out->us_timebase_div = (uint8_t)((rd2 & DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__MICROSECOND_TIME_BASE_DIV_MASK) >>
                                     DCN314_PWRSEQ_PANEL_PWRSEQ_REF_DIV2__MICROSECOND_TIME_BASE_DIV__SHIFT);

    /* Linux dce_get_16_bit_backlight_from_pwm(): mask period/duty by BITCNT, then scale to 0..0xFFFF. */
    bitcnt = out->period_bitcnt == 0u ? 16u : out->period_bitcnt;
    period_mask = (1u << bitcnt) - 1u;
    bl_period = out->period & period_mask;
    pwm_mask = period_mask << (16u - bitcnt);
    bl_pwm = out->fractional ? (out->active_int_frac & 0xFFFFu)
                             : (out->active_int_frac & pwm_mask);
    level = (uint64_t)bl_pwm << (1u + bitcnt);
    if (bl_period == 0u)
        bl_period = 0xFFFFu;
    level = level / bl_period;
    level = (level + 1u) >> 1;
    out->backlight_level = (uint32_t)level;
    return DCN314_OK;
}
