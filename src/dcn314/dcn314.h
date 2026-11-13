/*
 * dcn314.h - DCN 3.1.4 (Phoenix, RDNA3 780M) display register layer, STAGE 1 READ-ONLY SURVEY.
 *
 * Sibling of src/dcn41/ (the RDNA4 base), same shape, different silicon. This card delivers only the
 * read-only function set the stage-1 survey needs (OTG timing/position/frame_count, HUBP surface
 * address/format/tiling, DIO link + PWM state) plus the minimal AUX read transaction. There are NO
 * display-state write functions: stage 1 has zero state side effects (D-group constraint). The only
 * writes this layer can make are the AUX request-register writes, and those pass through a caller
 * supplied runtime whitelist (the whitelist body lives in T2's writeguard; this layer only calls it).
 *
 * Freestanding C11 that also compiles as C++17: no heap, no libc beyond <stdint.h>/<stddef.h>/<stdbool.h>.
 * Every register offset comes from the generated dcn314_regs.h (Linux v7.2 dcn_3_1_4_offset.h/_sh_mask.h).
 *
 * TODO(survey 回填): the DCN 3.1.4 segment bases are NOT measured. dcn314_dev_init accepts the caller's
 * discovery seg[] as-is (offsets are real; the base is the survey's job). The write gate is DENY BY
 * DEFAULT: only the generated allow ranges (built for the placeholder bases) can return OK, so every
 * write outside them - and every write while the guard is unarmed or the bases differ - is refused
 * until the survey regenerates the table.
 */
#ifndef N48_DCN314_H
#define N48_DCN314_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dcn314_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DCN314_NUM_SEGS 5u
#define DCN314_NUM_OTG 4u        /* OTG0..3 (dcn_3_1_4_offset.h) */
#define DCN314_NUM_HUBP 4u       /* HUBP0..3 / HUBPREQ0..3 */
#define DCN314_NUM_AUX 5u        /* DP_AUX0..4 */
#define DCN314_NUM_DIG 5u        /* DIG0..4 */
#define DCN314_NUM_DP 5u         /* DP0..4 */
#define DCN314_NUM_DIO_LINK 6u   /* DIO_LINKA..F */
#define DCN314_NUM_PWRSEQ 2u     /* PWRSEQ0..1 */

/* Placeholder segment bases used by the generated allowlist and the census. TODO(survey 回填): these
 * are synthetic, monotonic stand-ins; the stage-1 survey's DCN snapshot must replace them. */
#define DCN314_SEG0_PLACEHOLDER 0x00000000u
#define DCN314_SEG1_PLACEHOLDER 0x00001000u
#define DCN314_SEG2_PLACEHOLDER 0x00010000u
#define DCN314_SEG3_PLACEHOLDER 0x00020000u
#define DCN314_SEG4_PLACEHOLDER 0x00030000u

enum {
    DCN314_OK = 0,
    DCN314_E_ARG = -1,        /* null pointer or malformed argument */
    DCN314_E_INST = -2,       /* instance out of range for this call */
    DCN314_E_BASES = -3,      /* segment bases are unusable */
    DCN314_E_WINDOW = -4,     /* an address would leave the caller's BAR5 window */
    DCN314_E_UNINIT = -5,     /* dcn314_dev_init() did not succeed on this device */
    DCN314_E_NOWRITE = -6,    /* stage-1 survey is read-only: display write primitives are stubs */
    DCN314_E_DENY = -7,       /* the caller's runtime whitelist refused an AUX request write */
    DCN314_E_TIMEOUT = -8,    /* a bounded wait expired */
    DCN314_E_AUX_HPD = -9,    /* sink HPD disconnected */
    DCN314_E_AUX_NACK = -10,  /* sink NACKed the AUX request */
    DCN314_E_AUX_DEFER = -11, /* sink deferred the AUX request */
    DCN314_E_AUX_REPLY = -12, /* malformed / short AUX reply */
    DCN314_E_AUX_ERR = -13    /* AUX error/timeout status bit set */
};

typedef uint32_t (*dcn314_rreg_fn)(void *cookie, uint32_t abs_dword);
typedef void (*dcn314_wreg_fn)(void *cookie, uint32_t abs_dword, uint32_t value);
typedef void (*dcn314_udelay_fn)(void *cookie, uint32_t microseconds);

/* Runtime whitelist for the ONLY writes this layer makes (AUX request registers). true = allow.
 * The whitelist body lives in T2's writeguard; dcn314_allow_write_cb() is one such body. */
typedef bool (*dcn314_write_ok_fn)(void *cookie, uint32_t abs_dword, uint32_t value);

#define DCN314_DEV_MAGIC 0x34314e44u   /* "DN14" */
#define DCN314_BAD_ADDR 0xFFFFFFFFu

struct dcn314_dev {
    uint32_t magic;
    uint32_t flags;
    void *cookie;
    dcn314_rreg_fn rreg;
    dcn314_wreg_fn wreg;
    dcn314_udelay_fn udelay;
    dcn314_write_ok_fn write_ok;
    uint32_t seg[DCN314_NUM_SEGS];
    uint32_t mmio_dwords;   /* BAR5 size in dwords; every address used is checked against it */
};

/* Initialise: checks the callbacks and that seg[b] + the largest offset this layer uses for BASE_IDX b
 * is below mmio_dwords. No register I/O. write_ok may be NULL (then every write is refused). */
int dcn314_dev_init(struct dcn314_dev *dev, void *cookie, dcn314_rreg_fn rreg, dcn314_wreg_fn wreg,
                    dcn314_udelay_fn udelay, dcn314_write_ok_fn write_ok,
                    const uint32_t seg[DCN314_NUM_SEGS], uint32_t mmio_dwords, uint32_t flags);

/* Absolute BAR5 dword for (offset, base index); DCN314_BAD_ADDR when either is invalid. */
uint32_t dcn314_abs(const struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx);

/* Stage-1 write stubs: display state is read-only. Always DCN314_E_NOWRITE, never I/O. */
int dcn314_stage1_write_refused(void);

#ifdef __cplusplus
}
#endif
#endif /* N48_DCN314_H */
