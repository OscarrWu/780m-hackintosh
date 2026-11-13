#include "n48log_stub.h"

#include "n48log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char     g_buf[amdgpu::kN48LogBytes];
uint32_t g_used = 0;

namespace amdgpu {

void n48_logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    const uint32_t len = (uint32_t)((size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
    if (g_used + len + 1 > kN48LogBytes)
        return; // full: drop, same stop-appending semantics as the kernel ring
    memcpy(g_buf + g_used, line, len);
    g_used += len;
    if (g_buf[g_used - 1] != '\n')
        g_buf[g_used++] = '\n';
    if (g_used < kN48LogBytes)
        g_buf[g_used] = '\0';
}

} // namespace amdgpu

void n48log_host_reset(void)
{
    g_used = 0;
    g_buf[0] = '\0';
}

const char *n48log_host_text(void)
{
    return g_buf;
}

bool n48log_host_has_tag(const char *tag)
{
    if (!tag)
        return false;
    const size_t tlen = strlen(tag);
    /* Length semantics, same as the kernel's n48_log_read: scan [g_buf, g_buf+g_used)
     * line by line, never relying on a NUL terminator (the ring can be full). */
    const char *end = g_buf + g_used;
    const char *p = g_buf;
    while (p < end) {
        const size_t rem = (size_t)(end - p);
        if (rem >= tlen + 1 && strncmp(p, tag, tlen) == 0 && p[tlen] == ':')
            return true;
        const char *nl = (const char *)memchr(p, '\n', rem);
        if (!nl)
            break;
        p = nl + 1;
    }
    return false;
}
