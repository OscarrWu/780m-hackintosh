/*
 * dcn314_allow.h - the DCN 3.1.4 DISPLAY WRITE ALLOWLIST (DENY BY DEFAULT).
 *
 * Sibling of src/dcn41/dcn41_allow.h, same promise and same check order:
 *
 *   1. DENY BY DEFAULT: dcn314_allow_classify() refuses unless an explicit display range matches.
 *   2. INERT until dcn314_allow_init() succeeds; init refuses unless the caller's five segment bases
 *      equal the ones the generated range table was built for. TODO(survey 回填): that table currently
 *      holds PLACEHOLDER bases, so on real Phoenix hardware init will (correctly) stay unarmed until
 *      the survey regenerates it. This is the intended "bases moved -> disable writes" control.
 *   3. INDIRECT WINDOWS HARD-DENIED before the allow table (low BAR5 page = MM_INDEX/DATA + SMN windows).
 *   4. NAMED SMU/PSP BLOCKS denied before the allow table (placeholder for Phoenix, see the generator).
 *   5. COUNTERS + first-N samples of refusals and allows, each with address/value/seq/tag.
 *
 * The generated table lives in dcn314_allow_ranges.h and is included only by dcn314_allow.c.
 * Freestanding: no heap, no libc, no floating point.
 */
#ifndef N48_DCN314_ALLOW_H
#define N48_DCN314_ALLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dcn314_allow_reason {
    DCN314_ALLOW_OK = 0,
    DCN314_ALLOW_UNARMED,
    DCN314_ALLOW_WINDOW,
    DCN314_ALLOW_INDIRECT,
    DCN314_ALLOW_BLOCK,
    DCN314_ALLOW_RANGE,
    DCN314_ALLOW_REASON_COUNT
};

#define DCN314_ALLOW_SAMPLES 16u
#define DCN314_ALLOW_TAG_LEN 20u
#define DCN314_ALLOW_STATE_MAGIC 0x34314c41u   /* "AL14" */

struct dcn314_allow_sample {
    uint32_t abs_dword;
    uint32_t value;
    uint32_t seq;
    uint8_t reason;
    char tag[DCN314_ALLOW_TAG_LEN];
};

struct dcn314_allow_state {
    uint32_t magic;
    uint32_t armed;
    uint32_t mmio_dwords;
    uint64_t writes_seen;
    uint64_t allowed;
    uint64_t refused;
    uint64_t reads_seen;
    uint64_t by_reason[DCN314_ALLOW_REASON_COUNT];
    uint32_t n_refuse_samples;
    uint32_t n_allow_samples;
    uint32_t refuse_dropped;
    uint32_t last_refused_abs;
    uint8_t last_refused_reason;
    struct dcn314_allow_sample refuse[DCN314_ALLOW_SAMPLES];
    struct dcn314_allow_sample allow[DCN314_ALLOW_SAMPLES];
};

/* Arm the guard. DCN314_ALLOW_OK, or a refusal reason. On refusal the state is zeroed and UNARMED. */
int dcn314_allow_init(struct dcn314_allow_state *st, const uint32_t seg[5], uint32_t mmio_dwords);

/* Pure classification, no state. */
int dcn314_allow_classify(uint32_t abs_dword, uint32_t mmio_dwords,
                          const char **range_name, const char **why);

/* The gate. true only if the write may proceed; the caller MUST NOT touch hardware on false. */
bool dcn314_allow_write(struct dcn314_allow_state *st, uint32_t abs_dword, uint32_t value,
                        const char *tag);

/* Adapter matching dcn314_write_ok_fn, so the AUX transaction can use this guard as its whitelist. */
bool dcn314_allow_write_cb(void *cookie, uint32_t abs_dword, uint32_t value);

void dcn314_allow_note_read(struct dcn314_allow_state *st);
const char *dcn314_allow_reason_name(int reason);

uint32_t dcn314_allow_range_count(void);
int dcn314_allow_range_at(uint32_t index, uint32_t *abs_lo, uint32_t *abs_hi, uint32_t *base_idx,
                          const char **name);
uint32_t dcn314_allow_total_dwords(void);
uint32_t dcn314_allow_bar5_dwords(void);

#ifdef __cplusplus
}
#endif
#endif /* N48_DCN314_ALLOW_H */
