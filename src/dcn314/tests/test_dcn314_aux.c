/*
 * test_dcn314_aux.c - host unit tests for the minimal AUX read transaction, driven by a fake MMIO
 * backend. No hardware, no Linux.
 *
 * The fake models exactly the AUX_SW_DATA auto-increment contract the transaction relies on (byte 0
 * with INDEX=0/AUTOINCREMENT_DISABLE, then auto-incremented bytes), the SW_DONE handshake, and the
 * reply window, so the tests can assert the request bytes the layer programmed (command byte, 20-bit
 * address, length, payload) as well as the reply decode. A "never done" backend makes a removed
 * timeout bound hang, which run_planted_breaks.py catches with a wall-clock timeout.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn314.h"
#include "dcn314_aux_min.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MMIO_DWORDS 0x40000u
#define SEG2 0x10000u
static const uint32_t SEG[5] = {
    0x00000000u, 0x00001000u, 0x00010000u, 0x00020000u, 0x00030000u
};

#define A_CTRL  (SEG2 + DCN314_AUX_AUX_CONTROL(0))
#define A_SWCTL (SEG2 + DCN314_AUX_AUX_SW_CONTROL(0))
#define A_ARB   (SEG2 + DCN314_AUX_AUX_ARB_CONTROL(0))
#define A_INT   (SEG2 + DCN314_AUX_AUX_INTERRUPT_CONTROL(0))
#define A_STAT  (SEG2 + DCN314_AUX_AUX_SW_STATUS(0))
#define A_DATA  (SEG2 + DCN314_AUX_AUX_SW_DATA(0))

#define DONE_MASK    DCN314_AUX_AUX_SW_STATUS__AUX_SW_DONE_MASK
#define COUNT_MASK   DCN314_AUX_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT_MASK
#define COUNT_SHIFT  DCN314_AUX_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT__SHIFT
#define HPD_MASK     DCN314_AUX_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK
#define TMO_MASK     DCN314_AUX_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_STATE_MASK
#define DATA_MASK    DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_MASK
#define DATA_SHIFT   DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA__SHIFT
#define INDEX_MASK   DCN314_AUX_AUX_SW_DATA__AUX_SW_INDEX_MASK
#define INDEX_SHIFT  DCN314_AUX_AUX_SW_DATA__AUX_SW_INDEX__SHIFT
#define RW_MASK      DCN314_AUX_AUX_SW_DATA__AUX_SW_DATA_RW_MASK
#define AUTODIS_MASK DCN314_AUX_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK
#define GO_MASK      DCN314_AUX_AUX_SW_CONTROL__AUX_SW_GO_MASK
#define DONE_ACK     DCN314_AUX_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK
#define ARB_USE      DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK
#define ARB_DONE     DCN314_AUX_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK
#define ARB_STAT     DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK
#define ARB_STAT_SH  DCN314_AUX_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT
#define EN_MASK      DCN314_AUX_AUX_CONTROL__AUX_EN_MASK
#define RESET_MASK   DCN314_AUX_AUX_CONTROL__AUX_RESET_MASK
#define RESETD_MASK  DCN314_AUX_AUX_CONTROL__AUX_RESET_DONE_MASK

struct aux_model {
    uint32_t mem[MMIO_DWORDS];
    uint8_t req[32];
    uint32_t req_n, req_ptr;
    uint8_t last_req[32];
    uint32_t last_req_n;
    uint8_t window[32];
    uint32_t read_ptr;
    uint8_t reply_code;
    uint32_t force_count;
    bool hpd, timeout_state, never_done;
    uint8_t data[256];
    uint32_t i2c_off;
    uint32_t writes, reads, udelays, write_ok_calls;
    bool deny_all, deny_data, deny_release;
    uint32_t last_denied;
    bool model_reset, reset_stuck;
    uint32_t reset_writes;
};

static uint32_t fake_read(void *c, uint32_t a)
{
    struct aux_model *m = c;
    m->reads++;
    if (a == A_STAT) {
        uint32_t v = m->mem[a];
        if (m->never_done)
            v &= ~DONE_MASK;
        return v;
    }
    if (a == A_DATA) {
        uint32_t b = m->window[m->read_ptr & 31u];
        m->read_ptr++;
        return (b << DATA_SHIFT) & DATA_MASK;
    }
    return a < MMIO_DWORDS ? m->mem[a] : 0xdeadbeefu;
}

static void fake_build_reply(struct aux_model *m, uint32_t action, uint32_t len)
{
    uint32_t i, count;
    uint8_t code = m->reply_code;

    m->window[0] = (uint8_t)(code << 4);
    if (action == 0x90u) {
        count = 1u + len;
        for (i = 0; i < len; i++)
            m->window[1u + i] = m->data[i];
    } else if (action == 0x50u) {
        count = 1u + len;
        for (i = 0; i < len; i++)
            m->window[1u + i] = m->data[(m->i2c_off + i) & 0xffu];
        m->i2c_off += len;
    } else {
        count = 1u;
        if (action == 0x40u && m->req_n >= 5u)
            m->i2c_off = m->req[4];
    }
    m->mem[A_STAT] = ((m->force_count ? m->force_count : count) << COUNT_SHIFT) |
                     (m->hpd ? HPD_MASK : 0u) | (m->timeout_state ? TMO_MASK : 0u) |
                     (m->never_done ? 0u : DONE_MASK);
}

static void fake_write(void *c, uint32_t a, uint32_t v)
{
    struct aux_model *m = c;
    m->writes++;
    m->mem[a] = v;

    if (a == A_DATA) {
        if (v & RW_MASK) {
            m->read_ptr = (v & INDEX_MASK) >> INDEX_SHIFT;
        } else {
            uint32_t idx = (v & AUTODIS_MASK) ? ((v & INDEX_MASK) >> INDEX_SHIFT) : m->req_ptr;
            m->req[idx & 31u] = (uint8_t)((v & DATA_MASK) >> DATA_SHIFT);
            m->req_ptr = (idx + 1u) & 31u;
            if (m->req_ptr > m->req_n)
                m->req_n = m->req_ptr;
        }
    } else if (a == A_SWCTL && (v & GO_MASK)) {
        uint32_t action = m->req[0] & 0xf0u;
        uint32_t len = (m->req_n >= 4u) ? (uint32_t)m->req[3] + 1u : 0u;
        m->last_req_n = m->req_n;
        memcpy(m->last_req, m->req, sizeof m->last_req);
        fake_build_reply(m, action, len);
        m->req_n = 0u;
        m->req_ptr = 0u;
    } else if (a == A_INT && (v & DONE_ACK)) {
        m->mem[A_STAT] &= ~DONE_MASK;
    } else if (a == A_ARB) {
        uint32_t st = (v & ARB_USE) ? 1u : 0u;
        m->mem[A_ARB] = (v & ~ARB_STAT) | (st << ARB_STAT_SH);
    } else if (a == A_CTRL && m->model_reset) {
        m->reset_writes++;
        if (v & RESET_MASK)
            m->mem[A_CTRL] = m->reset_stuck ? (v & ~RESETD_MASK) : (v | RESETD_MASK);
        else
            m->mem[A_CTRL] = v & ~RESETD_MASK;
    }
}

static void fake_udelay(void *c, uint32_t us)
{
    struct aux_model *m = c;
    (void)us;
    m->udelays++;
}

static bool fake_write_ok(void *c, uint32_t a, uint32_t v)
{
    struct aux_model *m = c;
    m->write_ok_calls++;
    if (m->deny_all) {
        m->last_denied = a;
        return false;
    }
    if (m->deny_data && a == A_DATA) {
        m->last_denied = a;
        return false;
    }
    if (m->deny_release && a == A_ARB && !(v & ARB_USE)) {
        m->last_denied = a;
        return false;
    }
    return true;
}

static struct aux_model *new_model(void)
{
    struct aux_model *m = calloc(1, sizeof(*m));
    m->mem[A_CTRL] = EN_MASK;   /* engine already enabled: skip the reset handshake */
    return m;
}

static int init_dev(struct dcn314_dev *d, struct aux_model *m)
{
    return dcn314_dev_init(d, m, fake_read, fake_write, fake_udelay, fake_write_ok, SEG, MMIO_DWORDS, 0);
}

static void test_dpcd_ack(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;
    uint32_t i;

    for (i = 0; i < 8u; i++)
        m->data[i] = (uint8_t)(0xa0u + i);
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    CHECK(dcn314_abs(&d, DCN314_AUX_AUX_SW_DATA(0), 2u) == 0x11f56u, "AUX_SW_DATA abs 0x%x",
          dcn314_abs(&d, DCN314_AUX_AUX_SW_DATA(0), 2u));

    CHECK(dcn314_aux_dpcd_read(&d, 0, 0x00000u, 4u, &r) == DCN314_OK, "dpcd ok");
    CHECK(r.reply == DCN314_AUX_REPLY_ACK && r.bytes == 4u, "reply %d bytes %u", r.reply, r.bytes);
    CHECK(memcmp(r.data, m->data, 4) == 0, "data");
    CHECK(m->last_req_n >= 4u && m->last_req[0] == 0x90u && m->last_req[1] == 0x00u &&
          m->last_req[2] == 0x00u && m->last_req[3] == 3u, "request %02x %02x %02x %02x",
          m->last_req[0], m->last_req[1], m->last_req[2], m->last_req[3]);
    CHECK(m->writes > 0u && m->write_ok_calls >= m->writes, "all writes gated (%u/%u)",
          m->write_ok_calls, m->writes);
    CHECK(m->udelays == 0u, "no delay needed");

    /* 20-bit address composition: action 0x90 | address[19:16], then address[15:8], address[7:0]. */
    m->req_n = 0u;
    m->req_ptr = 0u;
    m->i2c_off = 0u;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0x12345u, 2u, &r) == DCN314_OK, "dpcd addr ok");
    CHECK(m->last_req[0] == 0x91u && m->last_req[1] == 0x23u && m->last_req[2] == 0x45u &&
          m->last_req[3] == 1u, "request20 %02x %02x %02x %02x", m->last_req[0],
          m->last_req[1], m->last_req[2], m->last_req[3]);

    free(m);
}

static void test_failures(void)
{
    struct dcn314_dev d;
    struct aux_model *m;
    struct dcn314_aux_result r;

    m = new_model();
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->reply_code = 0x1u;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_NACK, "nack rc");
    CHECK(r.reply == DCN314_AUX_REPLY_NACK, "nack reply");
    m->reply_code = 0x2u;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_DEFER, "defer rc");
    CHECK(r.reply == DCN314_AUX_REPLY_DEFER, "defer reply");
    free(m);

    m = new_model();
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->hpd = true;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_HPD, "hpd status rc");
    CHECK(r.reply == DCN314_AUX_REPLY_HPD_DISCON, "hpd status reply");
    free(m);

    m = new_model();
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->timeout_state = true;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_ERR, "rx timeout state");
    free(m);

    m = new_model();
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->never_done = true;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_TIMEOUT, "poll timeout");
    CHECK(r.reply == DCN314_AUX_REPLY_TIMEOUT, "timeout reply");
    CHECK(m->udelays > 0u, "bounded poll delayed");
    free(m);
}

static void test_deny(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;
    uint32_t before;

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->deny_all = true;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_DENY, "deny all rc");
    CHECK(m->write_ok_calls > 0u, "whitelist consulted");
    CHECK(m->writes == 0u, "no bus write after deny (%u)", m->writes);
    free(m);

    m = new_model();
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->deny_data = true;
    before = m->mem[A_DATA];
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_DENY, "deny sw_data rc");
    CHECK(m->last_denied == A_DATA && m->mem[A_DATA] == before, "sw_data untouched");
    free(m);

    m = new_model();
    CHECK(dcn314_dev_init(&d, m, fake_read, fake_write, fake_udelay, NULL, SEG, MMIO_DWORDS, 0) ==
          DCN314_OK, "init null whitelist");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_DENY, "null whitelist deny");
    free(m);
}

static void test_reply_truncation(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;
    uint32_t i;

    for (i = 0; i < 16u; i++)
        m->data[i] = (uint8_t)(0x30u + i);
    CHECK(init_dev(&d, m) == DCN314_OK, "init");

    /* 16 data bytes plus the status byte is the largest reply the result buffer can hold. */
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 16u, &r) == DCN314_OK, "16-byte read ok");
    CHECK(r.bytes == 16u && !r.truncated, "16-byte reply bytes %u truncated %d",
          r.bytes, r.truncated);
    CHECK(memcmp(r.data, m->data, 16) == 0, "16-byte data");

    /* An oversized reply-byte count must be clamped, never copied past data[16].
     * 31 is the largest value the 5-bit REPLY_BYTE_COUNT field can report (0x1f mask). */
    m->force_count = 31u;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 4u, &r) == DCN314_OK, "oversized reply ok");
    CHECK(r.bytes == 16u && r.truncated, "oversized reply bytes %u truncated %d",
          r.bytes, r.truncated);
    CHECK(memcmp(r.data, m->data, 4) == 0, "oversized reply keeps prefix");
    free(m);
}

static void test_release_deny(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;

    m->data[0] = 0x5au;
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    m->deny_release = true;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_DENY, "release deny rc");
    CHECK(m->last_denied == A_ARB, "release deny addressed A_ARB");
    CHECK(r.reply == DCN314_AUX_REPLY_ACK && r.bytes == 1u && r.data[0] == 0x5au,
          "reply kept despite release deny");
    /* A denied release is still observable when the transaction itself failed. */
    m->reply_code = 0x1u;
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_DENY, "release deny outranks nack");
    CHECK(r.reply == DCN314_AUX_REPLY_NACK, "transaction reply still recorded");
    free(m);
}

static void test_edid(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    uint8_t buf[256];
    uint32_t i;

    for (i = 0; i < 256u; i++)
        m->data[i] = (uint8_t)(i * 7u);
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    memset(buf, 0, sizeof(buf));
    CHECK(dcn314_aux_read_edid(&d, 0, buf, 128u) == DCN314_OK, "edid 128 rc");
    CHECK(memcmp(buf, m->data, 128) == 0, "edid 128 data");
    CHECK(m->i2c_off == 128u, "edid 128 offset %u", m->i2c_off);

    m->i2c_off = 0u;
    memset(buf, 0, sizeof(buf));
    CHECK(dcn314_aux_read_edid(&d, 0, buf, 256u) == DCN314_OK, "edid 256 rc");
    CHECK(memcmp(buf, m->data, 256) == 0, "edid 256 data");
    CHECK(m->i2c_off == 256u, "edid 256 offset %u", m->i2c_off);

    /* A short reply is an error, not a silent partial read. */
    m->i2c_off = 0u;
    m->reply_code = 0x1u;
    CHECK(dcn314_aux_read_edid(&d, 0, buf, 128u) == DCN314_E_AUX_NACK, "edid nack");
    free(m);
}

static void test_bad_args(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;
    uint8_t buf[256];

    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 0u, &r) == DCN314_E_ARG, "len 0");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 17u, &r) == DCN314_E_ARG, "len 17");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0x100000u, 1u, &r) == DCN314_E_ARG, "addr 21 bits");
    CHECK(dcn314_aux_dpcd_read(&d, 5, 0u, 1u, &r) == DCN314_E_INST, "aux 5");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, NULL) == DCN314_E_ARG, "null result");
    CHECK(dcn314_aux_dpcd_read(NULL, 0, 0u, 1u, &r) == DCN314_E_ARG, "null dev");
    CHECK(dcn314_aux_read_edid(&d, 0, buf, 100u) == DCN314_E_ARG, "edid 100");
    CHECK(dcn314_aux_read_edid(&d, 0, NULL, 128u) == DCN314_E_ARG, "edid null buf");
    CHECK(dcn314_aux_read_edid(&d, 5, buf, 128u) == DCN314_E_INST, "edid aux 5");
    free(m);
}

static void test_engine_reset(void)
{
    struct dcn314_dev d;
    struct aux_model *m = new_model();
    struct dcn314_aux_result r;

    m->mem[A_CTRL] = 0u;
    m->model_reset = true;
    m->data[0] = 0x5au;
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_OK, "reset path ok");
    CHECK((m->mem[A_CTRL] & EN_MASK) && !(m->mem[A_CTRL] & RESET_MASK),
          "engine enabled, reset released");
    CHECK(m->reset_writes == 2u, "reset writes %u", m->reset_writes);
    free(m);

    m = new_model();
    m->mem[A_CTRL] = 0u;
    m->model_reset = true;
    m->reset_stuck = true;
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_ERR, "reset timeout rc");
    CHECK(m->udelays > 0u, "reset wait delayed");
    CHECK(!(m->mem[A_ARB] & ARB_USE), "no arbitration after failed reset");
    free(m);

    m = new_model();
    m->mem[A_ARB] = (2u << ARB_STAT_SH);
    CHECK(init_dev(&d, m) == DCN314_OK, "init");
    CHECK(dcn314_aux_dpcd_read(&d, 0, 0u, 1u, &r) == DCN314_E_AUX_ERR, "dmcu owned rc");
    free(m);
}

int main(void)
{
    test_dpcd_ack();
    test_failures();
    test_deny();
    test_release_deny();
    test_reply_truncation();
    test_edid();
    test_engine_reset();
    test_bad_args();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
