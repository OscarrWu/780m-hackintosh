// LogChannel.h — the pure half of the N48R3LOG export channel (D7).
// Line assembly and registry chunking live here and are host-testable. The kernel-side
// IORegistry setProperty shell is deliberately NOT implemented in phase 1: T2 supplies
// it through TgtLogSink.
#ifndef N48_COMMON_LOGCHANNEL_H
#define N48_COMMON_LOGCHANNEL_H

#include <stddef.h>
#include <stdint.h>

// One registry property per chunk; 1 KiB keeps each property well inside the
// single-property size limit that motivated chunking in the first place.
constexpr uint32_t kTgtLogChunkBytes = 1024;

// Assemble one exported line: "<TGT_LOG_PREFIX> <tag>: <text>\n".
// snprintf-style: returns the would-be length excluding the NUL, NUL-terminates when
// cap > 0. A null tag becomes "-", a null text becomes "".
size_t tgt_log_line(char *out, size_t cap, const char *tag, const char *text);

// Number of chunks for len bytes: 0 bytes -> 0 chunks, otherwise ceil(len / chunk).
uint32_t tgt_log_chunk_count(size_t len);

// Byte range of chunk index. Returns false when index is out of range; otherwise
// writes the offset and the chunk length (the last chunk may be short).
bool tgt_log_chunk_span(size_t len, uint32_t index, size_t *offset, size_t *chunk_len);

// Registry property name for chunk index: "<TGT_LOG_PREFIX>-<%04u>".
// snprintf-style return value.
size_t tgt_log_chunk_key(char *out, size_t cap, uint32_t index);

// The kernel-side shell T2 implements. Phase 1 only fixes the interface.
typedef bool (*TgtLogSetProperty)(void *user, const char *key, const char *bytes, size_t len);

struct TgtLogSink {
    TgtLogSetProperty set_property;
    void *user;
};

#endif // N48_COMMON_LOGCHANNEL_H
