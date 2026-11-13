// LogChannel.cpp — see LogChannel.h. Pure logic only; T2 owns the setProperty shell.
#include "LogChannel.h"
#include "TargetConfig.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#endif

size_t tgt_log_line(char *out, size_t cap, const char *tag, const char *text)
{
    const char *t = tag ? tag : "-";
    const char *m = text ? text : "";
    return (size_t)snprintf(out, cap, "%s %s: %s\n", TGT_LOG_PREFIX, t, m);
}

uint32_t tgt_log_chunk_count(size_t len)
{
    if (len == 0) return 0;
    return (uint32_t)((len - 1) / kTgtLogChunkBytes + 1);
}

bool tgt_log_chunk_span(size_t len, uint32_t index, size_t *offset, size_t *chunk_len)
{
    if (index >= tgt_log_chunk_count(len)) return false;

    const size_t off = (size_t)index * kTgtLogChunkBytes;
    const size_t remain = len - off;
    if (offset) *offset = off;
    if (chunk_len) *chunk_len = remain < kTgtLogChunkBytes ? remain : kTgtLogChunkBytes;
    return true;
}

size_t tgt_log_chunk_key(char *out, size_t cap, uint32_t index)
{
    return (size_t)snprintf(out, cap, "%s-%04u", TGT_LOG_PREFIX, index);
}
