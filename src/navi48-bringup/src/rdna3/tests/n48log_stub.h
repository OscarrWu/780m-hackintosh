#pragma once

// Host stand-in for the n48log ring: the kernel links amd/n48log.cpp (2 MiB ring, read
// back via ReadLog); host test binaries link this stub, which captures the same
// "<tag>: <text>" lines into a buffer with the same append semantics (newline added,
// stop-appending when full) so tests can assert on the evidence channel.
void n48log_host_reset(void);
const char *n48log_host_text(void);
bool n48log_host_has_tag(const char *tag);
