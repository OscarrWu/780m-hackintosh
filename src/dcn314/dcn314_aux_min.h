/*
 * dcn314_aux_min.h - the minimal AUX read-only transaction (stage-4 dcn314_aux first stone).
 *
 * Scope (D6): DDC-over-AUX EDID read (128/256 B) and a few DPCD reads. This is deliberately the
 * SMALLEST correct transaction, NOT a full AUX engine: no writes in the sense of display programming -
 * the only writes are the AUX request-register writes needed to drive the hardware transaction, and
 * each of those passes through the caller-supplied runtime whitelist (dev->write_ok) BEFORE it hits
 * the bus. Stage 1 stays read-only at the display-state level; here we only read back sink data.
 *
 * No software retry loop: a transaction is attempted once with a bounded poll on AUX_SW_DONE, and a
 * failure (NACK/DEFER/HPD-disconnect/timeout/error) returns an error code. The caller decides whether
 * to retry. All MMIO is through dev->rreg/wreg with a hard upper bound on the busy-wait.
 */
#ifndef N48_DCN314_AUX_MIN_H
#define N48_DCN314_AUX_MIN_H

#include "dcn314.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed bound for one transaction. Linux derives REG_WAIT's try count from the hardware timeout;
 * this layer uses a fixed ceiling instead (~48 ms with POLL_US). */
#define DCN314_AUX_POLL_TRIES 30u
#define DCN314_AUX_POLL_US 1600u      /* ~one AUX frame budget at 1 Mbps */

/* Result of one transaction. */
enum dcn314_aux_reply {
    DCN314_AUX_REPLY_ACK = 0,
    DCN314_AUX_REPLY_NACK,
    DCN314_AUX_REPLY_DEFER,
    DCN314_AUX_REPLY_HPD_DISCON,
    DCN314_AUX_REPLY_TIMEOUT,
    DCN314_AUX_REPLY_ERROR
};

struct dcn314_aux_result {
    enum dcn314_aux_reply reply;
    uint8_t bytes;        /* valid bytes in data[] (clamped to sizeof(data)) */
    bool truncated;       /* reply carried more data bytes than data[] can hold */
    uint8_t data[16];     /* AUX SW data register holds 16 bytes per the register width */
};

/* Read up to `len` (1..16) bytes from DPCD address `addr` over AUX instance aux (0..4).
 * Returns DCN314_OK with the result, or a negative DCN314_E_* code on transport error. */
int dcn314_aux_dpcd_read(const struct dcn314_dev *dev, unsigned aux, uint32_t addr,
                         uint8_t len, struct dcn314_aux_result *out);

/* Read EDID via DDC-over-AUX. `buf` must hold `len` bytes (128 or 256). `len` must be 128 or 256.
 * Returns DCN314_OK on full read, or a negative DCN314_E_* code. */
int dcn314_aux_read_edid(const struct dcn314_dev *dev, unsigned aux, uint8_t *buf, uint16_t len);

/* The 7-bit DDC/EDID address on the AUX bus. */
#define DCN314_AUX_EDID_I2C_ADDR 0x50u

#ifdef __cplusplus
}
#endif
#endif /* N48_DCN314_AUX_MIN_H */
