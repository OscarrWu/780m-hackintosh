/*
 * dcn314_ro.h - DCN 3.1.4 READ-ONLY register readout functions (stage-1 survey direct dependencies).
 *
 * Every function here is a pure read over dcn314_read / dcn314_get. Survey (D6/D7/D8) reads: OTG timing
 * and scanout position, HUBP surface address/format/tiling, DIO link + PWM state. No writes, no locks.
 */
#ifndef N48_DCN314_RO_H
#define N48_DCN314_RO_H

#include "dcn314.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- OTG: timing + scanout position + frame counter (D6/D8) ---------- */

struct dcn314_otg_timing {
    uint32_t h_total;            /* number of pixel clocks in one line */
    uint32_t v_total;            /* number of lines in one frame       */
    uint32_t h_blank_start;      /* first blank pixel column           */
    uint32_t h_blank_end;        /* last blank pixel column            */
    uint32_t v_blank_start;      /* first blank line                   */
    uint32_t v_blank_end;        /* last blank line                    */
};

struct dcn314_otg_position {
    uint32_t vert_count;         /* current scanout line  */
    uint32_t horz_count;         /* current pixel column  */
    uint32_t vert_count_nom;     /* nominal vertical pos  */
};

/* Read the active timing of OTG i (0..3). Returns DCN314_E_INST for a bad instance. */
int dcn314_otg_read_timing(const struct dcn314_dev *dev, unsigned otg, struct dcn314_otg_timing *out);
/* Read the running scanout position of OTG i. */
int dcn314_otg_read_position(const struct dcn314_dev *dev, unsigned otg, struct dcn314_otg_position *out);
/* Current frame counter of OTG i (wraps at 0x80000000 per the register width). */
int dcn314_otg_read_frame_count(const struct dcn314_dev *dev, unsigned otg, uint32_t *out);
/* VSTARTUP / VUPDATE_NO_LOCK interrupt pending/enable snapshot (survey sync-status probing). */
int dcn314_otg_read_sync_status(const struct dcn314_dev *dev, unsigned otg,
                               bool *vstartup_pending, bool *vupdate_no_lock_pending);
/* Master-enable bit (OTG running vs powered down). */
int dcn314_otg_is_master_enabled(const struct dcn314_dev *dev, unsigned otg, bool *out);

/* ---------- HUBP: plane surface address/format/tiling (D6) ---------- */

struct dcn314_hubp_surface {
    uint64_t primary_address;        /* surf_addr + surf_addr_high */
    uint8_t  pixel_format;          /* SURFACE_PIXEL_FORMAT field  */
    uint8_t  sw_mode;                /* tiling swizzle mode         */
    uint8_t  meta_linear;            /* linear metadata (no DCC)    */
    uint8_t  pipe_aligned;           /* pipe-aligned tiling         */
    uint8_t  dcc_en;                 /* DCC compression enabled    */
    uint8_t  in_blank;               /* HUBP in blank              */
    uint16_t viewport_x;             /* primary viewport origin X   */
    uint16_t viewport_y;             /* primary viewport origin Y   */
    uint16_t viewport_w;             /* primary viewport width      */
    uint16_t viewport_h;             /* primary viewport height     */
};

/* Read the current plane surface state of HUBP i (0..3). */
int dcn314_hubp_read_surface(const struct dcn314_dev *dev, unsigned hubp,
                             struct dcn314_hubp_surface *out);

/* ---------- DIO: link status + PWM state (D7) ---------- */

/* Link encoder type selection for DIO link letter (0..5 => A..F). */
int dcn314_dio_link_read_type(const struct dcn314_dev *dev, unsigned link,
                              uint8_t *enc_type_sel, uint8_t *hpo_dp_enc_sel, uint8_t *hpo_hdmi_enc_sel);

/* DP link status (connected/embedded-panel/fec) per DP instance (0..4). */
int dcn314_dp_read_link_status(const struct dcn314_dev *dev, unsigned dp,
                               bool *link_up, bool *embedded_panel, bool *fec_active);
/* DIG front/back-end enable + sym clock status per DIG instance (0..4). */
int dcn314_dig_read_status(const struct dcn314_dev *dev, unsigned dig,
                           bool *fe_enabled, bool *be_enabled, bool *fe_symclk, bool *be_symclk);

/* DIO PWM (backlight) state per PWRSEQ instance (0..1). */
struct dcn314_pwm_state {
    bool   enabled;
    bool   fractional;
    uint32_t active_int_frac;        /* BL_ACTIVE_INT_FRAC_CNT (16-bit duty field) */
    uint32_t period;                 /* BL_PWM_PERIOD (16-bit period field)       */
    uint32_t period_bitcnt;          /* BL_PWM_PERIOD_BITCNT (0 => 16 per Linux)  */
    uint16_t ref_div;                /* BL_PWM_REF_DIV (16-bit field)             */
    uint8_t  xtal_ref_div;           /* XTAL_REF_DIV                          */
    uint8_t  us_timebase_div;        /* MICROSECOND_TIME_BASE_DIV             */
    /* Linux dce_get_16_bit_backlight_from_pwm(): 0..0xFFFF level (0 when no PWM) */
    uint32_t backlight_level;
};

int dcn314_pwm_read_state(const struct dcn314_dev *dev, unsigned pwrseq, struct dcn314_pwm_state *out);

#ifdef __cplusplus
}
#endif
#endif /* N48_DCN314_RO_H */
