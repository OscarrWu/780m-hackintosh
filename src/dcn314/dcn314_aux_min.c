/*
 * dcn314_aux_min.c - the minimal AUX read-only transaction. See dcn314_aux_min.h.
 *
 * Faithful to Linux v7.2 dce_aux.c (acquire_engine/release_engine, submit_channel_request,
 * get_channel_status, read_channel_reply): acquire the AUX engine, program the SW request registers
 * with the command byte / 20-bit address / optional length / write payload, arm AUX_SW_GO, bound-poll
 * AUX_SW_DONE, then decode the reply byte (high nibble) and copy the data bytes. There is no software
 * retry loop: one attempt, a bounded poll, and an error code on NACK/DEFER/HPD/timeout/invalid.
 *
 * Every AUX register WRITE (including the acquire/release arbitration writes) passes through
 * dev->write_ok BEFORE it reaches the bus; reads are ungated. The whitelist body is T2's writeguard.
 */
#include "dcn314_aux_min.h"
#include "dcn314_io.h"

#pragma GCC poison float double

/* Linux i2caux_transaction_action values (dc_ddc_types.h). */
#define AUX_ACTION_I2C_WRITE_MOT 0x40u
#define AUX_ACTION_I2C_READ_MOT  0x50u
#define AUX_ACTION_DP_READ       0x90u

/* Reply byte high-nibble codes (dc_ddc_types.h enum aux_transaction_reply; only the 4-bit codes the
 * AUX engine can report are reachable here - HPD is a separate AUX_SW_STATUS bit). */
#define AUX_REPLY_ACK                0x0u
#define AUX_REPLY_AUX_NACK           0x1u
#define AUX_REPLY_AUX_DEFER          0x2u
#define AUX_REPLY_I2C_OVER_AUX_NACK  0x4u
#define AUX_REPLY_I2C_OVER_AUX_DEFER 0x8u

/* The AUX_SW_DATA byte window (AUX_SW_INDEX is 5 bits, 0..31) and the DP AUX per-transaction max. */
#define AUX_DATA_WINDOW 32u
#define AUX_MAX_XFER    16u

#define AUX_DMCU_CAN_ACCESS_AUX 2u
#define AUX_SW_CAN_ACCESS_AUX   1u

static bool aux_valid(unsigned aux)
{
    return aux < DCN314_NUM_AUX;
}

/* Absolute dword for one AUX register, or DCN314_BAD_ADDR. */
static uint32_t aux_abs(const struct dcn314_dev *dev, unsigned aux, uint32_t offset,
                        uint32_t base_idx)
{
    if (!aux_valid(aux))
        return DCN314_BAD_ADDR;
    return dcn314_abs(dev, offset, base_idx);
}

/* Gated AUX register write: the caller's whitelist must approve this exact (abs, value). */
static int aux_write(const struct dcn314_dev *dev, uint32_t abs, uint32_t value)
{
    if (abs == DCN314_BAD_ADDR)
        return DCN314_E_INST;
    if (dev->write_ok == NULL || !dev->write_ok(dev->cookie, abs, value))
        return DCN314_E_DENY;
    dev->wreg(dev->cookie, abs, value);
    return DCN314_OK;
}

/* Submit one AUX request exactly as Linux submit_channel_request() does. */
static int aux_submit(const struct dcn314_dev *dev, unsigned aux, uint32_t action,
                      uint32_t address, uint8_t len, const uint8_t *data, bool is_write)
{
    uint32_t length;
    uint32_t value;
    uint32_t sw_data = aux_abs(dev, aux, DCN314_AUX_AUX_SW_DATA(aux),
                               DCN314_AUX_AUX_SW_DATA_BASE_IDX);
    int rc;

    if (sw_data == DCN314_BAD_ADDR)
        return DCN314_E_INST;

    /* Clear the DONE ack, then wait for DONE to fall (Linux REG_UPDATE + REG_WAIT). */
    rc = aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_INTERRUPT_CONTROL(aux),
                                DCN314_AUX_AUX_INTERRUPT_CONTROL_BASE_IDX),
                   DCN314_AUX_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK);
    if (rc != DCN314_OK)
        return rc;
    rc = dcn314_wait(dev, DCN314_AUX_AUX_SW_STATUS(aux), DCN314_AUX_AUX_SW_STATUS_BASE_IDX,
                     DCN314_AUX_AUX_SW_STATUS__AUX_SW_DONE_MASK,
                     DCN314_AUX_AUX_SW_STATUS__AUX_SW_DONE__SHIFT, 0u, false,
                     DCN314_AUX_POLL_TRIES, DCN314_AUX_POLL_US, NULL);
    if (rc != DCN314_OK)
        return rc;

    length = len ? 4u : 3u;
    if (is_write)
        length += len;

    /* START_DELAY=0, WR_BYTES=length. */
    value = (length << DCN314_AUX_AUX_SW_CONTROL__AUX_SW_WR_BYTES__SHIFT) &
            DCN314_AUX_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK;
    rc = aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_SW_CONTROL(aux),
                                DCN314_AUX_AUX_SW_CONTROL_BASE_IDX), value);
    if (rc != DCN314_OK)
        return rc;

    /* byte 0: command | address[19:16]; index 0, write, auto-increment disabled. */
    value = DCN314_AUX_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK |
            (((action | ((address & 0xF0000u) >> 16)) <<
              DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT) &
             DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK);
    rc = aux_write(dev, sw_data, value);
    if (rc != DCN314_OK)
        return rc;

    /* byte 1: address[15:8]; auto-increment now enabled. */
    value = (((address & 0xFF00u) >> 8) << DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT) &
            DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK;
    rc = aux_write(dev, sw_data, value);
    if (rc != DCN314_OK)
        return rc;

    /* byte 2: address[7:0]. */
    value = ((address & 0xFFu) << DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT) &
            DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK;
    rc = aux_write(dev, sw_data, value);
    if (rc != DCN314_OK)
        return rc;

    /* byte 3 (only when a length is present): length - 1. */
    if (len) {
        value = (((uint32_t)len - 1u) << DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT) &
                DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK;
        rc = aux_write(dev, sw_data, value);
        if (rc != DCN314_OK)
            return rc;
    }

    /* Write payload bytes, auto-incremented. */
    if (is_write) {
        uint32_t i;
        for (i = 0; i < len; i++) {
            value = ((uint32_t)data[i] << DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT) &
                    DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK;
            rc = aux_write(dev, sw_data, value);
            if (rc != DCN314_OK)
                return rc;
        }
    }

    /* GO=1, preserving WR_BYTES/START_DELAY. */
    value = DCN314_AUX_AUX_SW_CONTROL__AUX_SW_GO_MASK |
            ((length << DCN314_AUX_AUX_SW_CONTROL__AUX_SW_WR_BYTES__SHIFT) &
             DCN314_AUX_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK);
    return aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_SW_CONTROL(aux),
                                  DCN314_AUX_AUX_SW_CONTROL_BASE_IDX), value);
}

/* Read `n` reply bytes (status byte first) out of the SW_DATA window. */
static int aux_read_bytes(const struct dcn314_dev *dev, unsigned aux, uint8_t n, uint8_t *dst)
{
    uint32_t sw_data = aux_abs(dev, aux, DCN314_AUX_AUX_SW_DATA(aux),
                               DCN314_AUX_AUX_SW_DATA_BASE_IDX);
    uint32_t value;
    uint32_t i;
    int rc;

    if (sw_data == DCN314_BAD_ADDR || n == 0u || n > AUX_DATA_WINDOW)
        return DCN314_E_ARG;

    /* Select index 0 for reading (RW=1, auto-increment disabled) - a gated write. */
    value = DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_RW_MASK |
            DCN314_AUX_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK;
    rc = aux_write(dev, sw_data, value);
    if (rc != DCN314_OK)
        return rc;

    for (i = 0; i < n; i++) {
        uint32_t rv = dev->rreg(dev->cookie, sw_data);
        dst[i] = (uint8_t)((rv & DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK) >>
                           DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT);
    }
    return DCN314_OK;
}

/* Bound-poll DONE, decode the reply byte, copy the data bytes (Linux get_channel_status +
 * read_channel_reply). */
static int aux_collect_reply(const struct dcn314_dev *dev, unsigned aux,
                             struct dcn314_aux_result *out)
{
    uint32_t status;
    uint32_t n;
    uint32_t code;
    uint32_t data_len;
    uint32_t i;
    uint8_t bytes[AUX_DATA_WINDOW];
    int rc;

    out->truncated = false;

    rc = dcn314_wait(dev, DCN314_AUX_AUX_SW_STATUS(aux), DCN314_AUX_AUX_SW_STATUS_BASE_IDX,
                     DCN314_AUX_AUX_SW_STATUS__AUX_SW_DONE_MASK,
                     DCN314_AUX_AUX_SW_STATUS__AUX_SW_DONE__SHIFT, 1u, false,
                     DCN314_AUX_POLL_TRIES, DCN314_AUX_POLL_US, NULL);
    if (rc != DCN314_OK) {
        out->reply = DCN314_AUX_REPLY_TIMEOUT;
        return DCN314_E_TIMEOUT;
    }

    status = dcn314_read(dev, DCN314_AUX_AUX_SW_STATUS(aux), DCN314_AUX_AUX_SW_STATUS_BASE_IDX);
    if (status & DCN314_AUX_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK) {
        out->reply = DCN314_AUX_REPLY_HPD_DISCON;
        return DCN314_E_AUX_HPD;
    }
    if (status & (DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK |
                  DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_STATE_MASK)) {
        out->reply = DCN314_AUX_REPLY_TIMEOUT;
        return DCN314_E_AUX_ERR;
    }
    if (status & (DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_INVALID_STOP_MASK |
                  DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_RECV_NO_DET_MASK |
                  DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_H_MASK |
                  DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_L_MASK)) {
        out->reply = DCN314_AUX_REPLY_ERROR;
        return DCN314_E_AUX_REPLY;
    }

    n = (status & DCN314_AUX_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT_MASK) >>
        DCN314_AUX_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT__SHIFT;
    if (n == 0u || n > AUX_DATA_WINDOW) {
        out->reply = DCN314_AUX_REPLY_ERROR;
        return DCN314_E_AUX_REPLY;
    }

    rc = aux_read_bytes(dev, aux, (uint8_t)n, bytes);
    if (rc != DCN314_OK) {
        out->reply = DCN314_AUX_REPLY_ERROR;
        return rc;
    }

    code = (uint32_t)bytes[0] >> 4;
    if (code == AUX_REPLY_ACK) {
        out->reply = DCN314_AUX_REPLY_ACK;
        data_len = n - 1u;
        out->bytes = (uint8_t)data_len;
        if (data_len > sizeof(out->data)) {
            out->bytes = (uint8_t)sizeof(out->data);
            out->truncated = true;
        }
        for (i = 0; i < out->bytes; i++)
            out->data[i] = bytes[i + 1u];
        return DCN314_OK;
    }
    if (code == AUX_REPLY_AUX_NACK || code == AUX_REPLY_I2C_OVER_AUX_NACK) {
        out->reply = DCN314_AUX_REPLY_NACK;
        return DCN314_E_AUX_NACK;
    }
    if (code == AUX_REPLY_AUX_DEFER || code == AUX_REPLY_I2C_OVER_AUX_DEFER) {
        out->reply = DCN314_AUX_REPLY_DEFER;
        return DCN314_E_AUX_DEFER;
    }
    out->reply = DCN314_AUX_REPLY_ERROR;
    return DCN314_E_AUX_REPLY;
}

/* Acquire the AUX register block (Linux acquire_engine), every write gated. */
static int aux_acquire(const struct dcn314_dev *dev, unsigned aux)
{
    uint32_t ctl;
    uint32_t arb;
    uint32_t st;
    int rc;

    if (!aux_valid(aux))
        return DCN314_E_INST;

    st = dcn314_get(dev, DCN314_AUX_AUX_ARB_CONTROL(aux), DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX,
                    DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK,
                    DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT);
    if (st == AUX_DMCU_CAN_ACCESS_AUX)
        return DCN314_E_AUX_ERR;

    ctl = dcn314_read(dev, DCN314_AUX_AUX_CONTROL(aux), DCN314_AUX_AUX_CONTROL_BASE_IDX);
    if ((ctl & DCN314_AUX_AUX_CONTROL__AUX_EN_MASK) == 0u) {
        uint32_t v = ctl | DCN314_AUX_AUX_CONTROL__AUX_EN_MASK |
                     DCN314_AUX_AUX_CONTROL__AUX_RESET_MASK;
        rc = aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_CONTROL(aux),
                                    DCN314_AUX_AUX_CONTROL_BASE_IDX), v);
        if (rc != DCN314_OK)
            return rc;
        rc = dcn314_wait(dev, DCN314_AUX_AUX_CONTROL(aux), DCN314_AUX_AUX_CONTROL_BASE_IDX,
                         DCN314_AUX_AUX_CONTROL__AUX_RESET_DONE_MASK,
                         DCN314_AUX_AUX_CONTROL__AUX_RESET_DONE__SHIFT, 1u, false, 11u, 1u, NULL);
        if (rc != DCN314_OK)
            return DCN314_E_AUX_ERR;
        v = ctl | DCN314_AUX_AUX_CONTROL__AUX_EN_MASK;
        rc = aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_CONTROL(aux),
                                    DCN314_AUX_AUX_CONTROL_BASE_IDX), v);
        if (rc != DCN314_OK)
            return rc;
        rc = dcn314_wait(dev, DCN314_AUX_AUX_CONTROL(aux), DCN314_AUX_AUX_CONTROL_BASE_IDX,
                         DCN314_AUX_AUX_CONTROL__AUX_RESET_DONE_MASK,
                         DCN314_AUX_AUX_CONTROL__AUX_RESET_DONE__SHIFT, 0u, false, 11u, 1u, NULL);
        if (rc != DCN314_OK)
            return DCN314_E_AUX_ERR;
    }

    arb = dcn314_read(dev, DCN314_AUX_AUX_ARB_CONTROL(aux), DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX);
    arb |= DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK;
    arb &= ~DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK;
    rc = aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_ARB_CONTROL(aux),
                                DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX), arb);
    if (rc != DCN314_OK)
        return rc;

    st = dcn314_get(dev, DCN314_AUX_AUX_ARB_CONTROL(aux), DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX,
                    DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK,
                    DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT);
    if (st != AUX_SW_CAN_ACCESS_AUX)
        return DCN314_E_AUX_ERR;
    return DCN314_OK;
}

/* Best-effort release (Linux release_engine). A denied write is returned so the caller can observe
 * that the AUX block may still be held; the caller propagates it ahead of the transaction result. */
static int aux_release(const struct dcn314_dev *dev, unsigned aux)
{
    uint32_t arb;
    if (!aux_valid(aux))
        return DCN314_E_INST;
    arb = dcn314_read(dev, DCN314_AUX_AUX_ARB_CONTROL(aux), DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX);
    arb |= DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK;
    arb &= ~DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK;
    return aux_write(dev, aux_abs(dev, aux, DCN314_AUX_AUX_ARB_CONTROL(aux),
                                  DCN314_AUX_AUX_ARB_CONTROL_BASE_IDX), arb);
}

static int aux_one_read(const struct dcn314_dev *dev, unsigned aux, uint32_t action,
                        uint32_t address, uint8_t len, struct dcn314_aux_result *out)
{
    int rc = aux_submit(dev, aux, action, address, len, NULL, false);
    if (rc == DCN314_OK)
        rc = aux_collect_reply(dev, aux, out);
    return rc;
}

int dcn314_aux_dpcd_read(const struct dcn314_dev *dev, unsigned aux, uint32_t addr,
                         uint8_t len, struct dcn314_aux_result *out)
{
    int rc;
    int rel;

    if (dev == NULL || out == NULL)
        return DCN314_E_ARG;
    out->reply = DCN314_AUX_REPLY_ERROR;
    out->bytes = 0u;
    out->truncated = false;
    if (!aux_valid(aux))
        return DCN314_E_INST;
    if (len == 0u || len > AUX_MAX_XFER || addr > 0xFFFFFu)
        return DCN314_E_ARG;

    rc = aux_acquire(dev, aux);
    if (rc != DCN314_OK)
        return rc;
    rc = aux_one_read(dev, aux, AUX_ACTION_DP_READ, addr, len, out);
    rel = aux_release(dev, aux);
    /* A denied release outranks the transaction result: the AUX block may still be held. The
     * transaction outcome itself remains available in out->reply. */
    return rel != DCN314_OK ? rel : rc;
}

int dcn314_aux_read_edid(const struct dcn314_dev *dev, unsigned aux, uint8_t *buf, uint16_t len)
{
    struct dcn314_aux_result r;
    uint16_t done = 0u;
    uint32_t i;
    int rc;
    int rel;

    if (dev == NULL || buf == NULL)
        return DCN314_E_ARG;
    if (len != 128u && len != 256u)
        return DCN314_E_ARG;
    if (!aux_valid(aux))
        return DCN314_E_INST;

    rc = aux_acquire(dev, aux);
    if (rc != DCN314_OK)
        return rc;

    /* Set the sink's EDID offset to 0; the sink's pointer auto-increments across the chunked reads. */
    {
        uint8_t off = 0x00u;
        rc = aux_submit(dev, aux, AUX_ACTION_I2C_WRITE_MOT, DCN314_AUX_EDID_I2C_ADDR, 1u,
                        &off, true);
    }
    if (rc == DCN314_OK) {
        r.reply = DCN314_AUX_REPLY_ERROR;
        r.bytes = 0u;
        rc = aux_collect_reply(dev, aux, &r);
    }

    while (rc == DCN314_OK && done < len) {
        uint16_t chunk = (uint16_t)(len - done);
        if (chunk > AUX_MAX_XFER)
            chunk = AUX_MAX_XFER;
        r.reply = DCN314_AUX_REPLY_ERROR;
        r.bytes = 0u;
        rc = aux_one_read(dev, aux, AUX_ACTION_I2C_READ_MOT, DCN314_AUX_EDID_I2C_ADDR,
                          (uint8_t)chunk, &r);
        if (rc != DCN314_OK)
            break;
        if (r.bytes != chunk) {
            rc = DCN314_E_AUX_REPLY;
            break;
        }
        for (i = 0; i < chunk; i++)
            buf[done + i] = r.data[i];
        done = (uint16_t)(done + chunk);
    }

    rel = aux_release(dev, aux);
    return rel != DCN314_OK ? rel : rc;
}
