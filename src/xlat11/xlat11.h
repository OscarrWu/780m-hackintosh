/* xlat11 - gfx10.3 (Navi21) -> gfx11 (Navi3x / RDNA3) render-state PM4 translator.
 *
 * Stage-1 skeleton, sibling of src/xlat12 (gfx10.3 -> gfx12). The contract is
 * the same: plain C that also compiles as C++; in the translation path there
 * are no exceptions, RTTI, STL, heap allocation or floating point. One
 * function over caller-owned dword buffers.
 *
 * The input is always Apple-authored gfx10.3 PM4, so every register address is
 * read as gfx10.3 and re-emitted at its gfx11 address. Classes come from the
 * generated table xlat11_tables.h (Mesa gfx103.json / gfx11.json, cross-checked
 * against Linux gc_10_3_0 / gc_11_0_0):
 *   identical     address and fields equal: value re-emitted unchanged
 *   moved         address changed, fields equal: value re-emitted at gfx11 address
 *   field_repack  fields changed: value remapped by the generated per-register function
 *   absent        no gfx11 register: dropped (reason string in the table)
 *   reused        no gfx11 register of that NAME, but a DIFFERENT gfx11 register sits at the same
 *                 address: the value is mapped by the row's function and emitted there (xlat11_reused.h)
 *   legacy_vs     legacy VS slot: dropped under the NGG rule below, else refused
 *   unknown       neither gfx10.3 source names it: the whole call is refused
 *
 * NGG rule. gfx11 has only the NGG vertex path. A VGT_SHADER_STAGES_EN write whose
 * gfx10.3 PRIMGEN_EN bit is 0 describes a legacy-VS pipeline and is refused
 * (XLAT11_ERR_LEGACY_VS); PRIMGEN_EN=1 records NGG in the context. A legacy-VS
 * register write is dropped when the context is NGG or the value is 0, and is
 * refused (XLAT11_ERR_VS_MODE_UNKNOWN) otherwise.
 *
 * Memory-loaded state (LOAD_CONTEXT_REG, LOAD_SH_REG, LOAD_UCONFIG_REG, their
 * _INDEX forms, CLEAR_STATE, CONTEXT_CONTROL) is detected and refused with
 * XLAT11_ERR_MEMLOADED: a documented stub, no translation yet.
 *
 * SET_CONTEXT_REG / SET_SH_REG / SET_UCONFIG_REG runs are split wherever the gfx11
 * addresses stop being contiguous or change register block. SET_SH_REG_INDEX and
 * SET_UCONFIG_REG_INDEX are re-emitted one register per packet with the index kept.
 * The input header's flag byte (predicate, shader type, reset-filter-cam) is kept.
 * Every other PM4 type-3 packet is copied verbatim.
 *
 * Stage-1 scope (B5): the generated table covers the core draw / shader path
 * only (gen/skeleton_registers.txt; the coverage counts are in
 * xlat11_provenance.h). Full gfx10.3 coverage is a stage-3 pre-integration task.
 */
#ifndef XLAT11_H
#define XLAT11_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XLAT11_OK                  = 0,
    XLAT11_ERR_ARG             = 1,  /* NULL buffer with a non-zero size */
    XLAT11_ERR_BAD_PACKET      = 2,  /* not a PM4 type-3 packet */
    XLAT11_ERR_TRUNCATED       = 3,  /* packet runs past the end of the input */
    XLAT11_ERR_RANGE           = 4,  /* register offset/count outside its block */
    XLAT11_ERR_BAD_INDEX       = 5,  /* _INDEX form: index or offset bits not allowed */
    XLAT11_ERR_UNKNOWN_REG     = 6,  /* register with no authoritative mapping */
    XLAT11_ERR_LEGACY_VS       = 7,  /* VGT_SHADER_STAGES_EN describes a non-NGG pipeline */
    XLAT11_ERR_VS_MODE_UNKNOWN = 8,  /* non-zero legacy-VS write before NGG is known */
    XLAT11_ERR_MEMLOADED       = 9,  /* memory-loaded state packet: stub, not translated */
    XLAT11_ERR_CAPACITY        = 10  /* output buffer too small */
} xlat11_status;

enum { XLAT11_VS_UNKNOWN = 0, XLAT11_VS_NGG = 1 };

/* Register classes; the generated table (xlat11_tables.h) uses these values. A
 * field-changed register whose gfx10.3 and gfx11 field lists share no field name
 * has no established value mapping and is generated as UNKNOWN, not as a repack. */
typedef enum {
    XLAT11_CLS_IDENTICAL = 0, XLAT11_CLS_MOVED = 1, XLAT11_CLS_FIELD_REPACK = 2,
    XLAT11_CLS_ABSENT = 3, XLAT11_CLS_LEGACY_VS = 4, XLAT11_CLS_UNKNOWN = 5,
    /* REUSED: the gfx10.3 register has NO gfx11 namesake, but gfx11 puts a DIFFERENT
     * register at the SAME MMIO address. "absent" would drop the write and leave that
     * gfx11 register holding whatever its context slot last held. The row's `g11` is
     * that same address, its `repack` maps the gfx10.3 value to a gfx11 value for the
     * register that really lives there, and its `name` names BOTH. The rows live in
     * xlat11_reused.h (hand-written, NOT generated). */
    XLAT11_CLS_REUSED = 6
} Xlat11Class;
#define XLAT11_CLS_MAX XLAT11_CLS_REUSED

/* State carried across calls (e.g. from the pipeline IB to a later draw IB).
 * Zero-initialise. */
typedef struct {
    uint32_t vs_mode;
} xlat11_ctx;

typedef struct {
    uint32_t packets_in;          /* type-3 packets consumed */
    uint32_t packets_out;         /* type-3 packets emitted */
    uint32_t packets_verbatim;    /* copied unchanged */
    uint32_t set_packets_in;      /* SET_* and SET_*_INDEX packets consumed */
    uint32_t set_packets_out;     /* SET_* and SET_*_INDEX packets emitted */
    uint32_t set_packets_empty;   /* SET_* packets that emitted nothing */
    uint32_t set_runs_split;      /* extra output SET packets created by splitting */
    uint32_t regs_in;
    uint32_t regs_identical;
    uint32_t regs_moved;
    uint32_t regs_repacked;
    uint32_t regs_dropped_absent;
    uint32_t regs_reused;         /* XLAT11_CLS_REUSED - emitted at the same address as a DIFFERENT gfx11 register */
    uint32_t regs_dropped_legacy_vs;
    uint32_t dwords_out;          /* == *out_dwords on success */
    /* on error */
    uint32_t err_in_dword;        /* input dword index of the failing packet */
    uint32_t err_reg_addr;        /* gfx10.3 address involved, if any */
    uint32_t err_opcode;          /* PKT3 opcode (or the raw header for BAD_PACKET) */
    const char *err_name;         /* register or opcode name, if any */
} xlat11_stats;

/* Translate `in_dwords` dwords of gfx10.3 PM4 into `out` (capacity `out_cap`
 * dwords). On XLAT11_OK *out_dwords is the output length. `ctx` and `stats` may
 * be NULL (a NULL ctx means the NGG mode is unknown and nothing is recorded).
 * Nothing is written past out[out_cap-1]. On error the output is incomplete and
 * must be discarded; `stats` names the failing packet, register or opcode. */
xlat11_status xlat11_translate(xlat11_ctx *ctx, const uint32_t *in, uint32_t in_dwords,
                               uint32_t *out, uint32_t out_cap,
                               uint32_t *out_dwords, xlat11_stats *stats);

/* One gfx10.3 address: returns 1 and fills gfx11 address / class if tabled, else 0. */
int xlat11_lookup(uint32_t g10_addr, uint32_t *out_g11, uint32_t *out_cls);

/* The field repack for one register value (identity for non-repack classes). */
uint32_t xlat11_repack_value(uint32_t g10_addr, uint32_t value);

/* Table introspection for tests and integration probes. */
uint32_t xlat11_table_len(void);
int xlat11_table_entry(uint32_t i, uint32_t *g10, uint32_t *g11, uint32_t *cls, const char **name);

const char *xlat11_status_name(xlat11_status s);

#ifdef __cplusplus
}
#endif
#endif /* XLAT11_H */
