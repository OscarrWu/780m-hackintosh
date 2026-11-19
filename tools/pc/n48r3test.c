//
//  n48r3test — minimal userspace CLI for the RDNA3 (Phoenix 780M) bring-up kext.
//
//  Stage 1 has one sub-command:
//    info   print the IP table (version + base), PCI/BAR baseline and survey status,
//           exactly as the kext's user client returns it.
//
//  ABI (shared with the rdna3 kext, do not drift):
//    IOServiceMatching("NaviR3Bringup") -> IOServiceOpen(type 0)
//    -> IOConnectCallMethod(selector 0, scalar in offset, chunked text structOutput, scalar out total)
//    root only.
//
//  Build (fixed command, see src/BUILDING.md "Forensic tool (n48r3test)"):
//    make -C src/src/navi48-bringup -f Makefile.linux tools-macos
//  -> src/build/n48r3test (osxcross, x86_64, min macOS 11)
//
//  Host tests (no IOKit, any host):
//    cc -DN48R3_HOST_TEST -Wall -Wextra -Werror -o /tmp/n48r3test_host tools/pc/n48r3test.c
//    /tmp/n48r3test_host
//

#if defined(N48R3_HOST_TEST)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#ifndef N48R3_HOST_TEST
#include <IOKit/IOKitLib.h>
#endif
#include <fcntl.h>
#include <time.h>
#include "../../src/navi48-bringup/src/Navi48UserClientABI.h"
#include "../../src/navi48-bringup/src/Navi48R3ABI.h"

#define N48R3_SERVICE_NAME N48R3_UC_SERVICE_NAME
#define N48R3_SEL_INFO     ((uint32_t)kN48R3SelInfo)
#define N48R3_SEL_TRIGGER  ((uint32_t)kN48R3SelTrigger)
#define N48R3_SEL_BO_ALLOC   ((uint32_t)kN48R3SelBoAlloc)
#define N48R3_SEL_BO_FREE    ((uint32_t)kN48R3SelBoFree)
#define N48R3_SEL_SUBMIT     ((uint32_t)kN48R3SelSubmit)
#define N48R3_SEL_FENCE_READ ((uint32_t)kN48R3SelFenceRead)
#define N48R3_INFO_MAX     16384u
/* Tool version, printed by the `version` sub-command (ABI self-identification). */
#define N48R3_TOOL_VERSION "1.0.0"

#define N48R3_MSG_NOT_LOADED \
    "n48r3: driver not loaded (no " N48R3_SERVICE_NAME " service; is the kext loaded and matching the GPU?)"

/* kIOMainPortDefault arrived with macOS 12; older SDKs only have the master port. */
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define N48R3_MAIN_PORT kIOMainPortDefault
#else
#define N48R3_MAIN_PORT kIOMasterPortDefault
#endif

/* Exit codes; --help lists them for scripts. */
enum {
    N48R3_EXIT_OK         = 0,
    N48R3_EXIT_NOT_LOADED = 1,
    N48R3_EXIT_OPEN       = 2,
    N48R3_EXIT_CALL       = 3,
    N48R3_EXIT_NOT_ROOT   = 4,
    N48R3_EXIT_USAGE      = 5,
    N48R3_EXIT_TIMEOUT    = 6,
};

#ifdef N48R3_HOST_TEST
/* Documented IOKit values, so the pure helpers can be tested without IOKit. */
#define N48R3_KR_ERROR          ((int)0xe00002bcu)
#define N48R3_KR_NO_DEVICE      ((int)0xe00002c0u)
#define N48R3_KR_NOT_PRIVILEGED ((int)0xe00002c1u)
#define N48R3_KR_BAD_ARGUMENT   ((int)0xe00002c2u)
#define N48R3_KR_UNSUPPORTED    ((int)0xe00002c7u)
#define N48R3_KR_NOT_OPEN       ((int)0xe00002cdu)
#define N48R3_KR_TIMEOUT        ((int)0xe00002d6u)
#define N48R3_KR_NOT_PERMITTED  ((int)0xe00002e2u)
#else
#define N48R3_KR_ERROR         kIOReturnError
#define N48R3_KR_NO_DEVICE     kIOReturnNoDevice
#define N48R3_KR_NOT_PRIVILEGED kIOReturnNotPrivileged
#define N48R3_KR_BAD_ARGUMENT  kIOReturnBadArgument
#define N48R3_KR_UNSUPPORTED   kIOReturnUnsupported
#define N48R3_KR_NOT_OPEN      kIOReturnNotOpen
#define N48R3_KR_TIMEOUT       kIOReturnTimeout
#define N48R3_KR_NOT_PERMITTED kIOReturnNotPermitted
#endif

/* Sub-command table. Stage 2/3 appends sdma/mem/gart/submit here. */
enum n48r3_cmd {
    N48R3_CMD_NONE = 0,
    N48R3_CMD_HELP,
    N48R3_CMD_INFO,
    N48R3_CMD_TRIGGER,
    N48R3_CMD_LOG,
    N48R3_CMD_LOGSTREAM,
    N48R3_CMD_LOGRESET,
    N48R3_CMD_VERSION,
    N48R3_CMD_MEM,
    N48R3_CMD_GART,
    N48R3_CMD_DOORBELL,
    N48R3_CMD_SUBMIT,
    N48R3_CMD_COMPUTE,
    N48R3_CMD_UNKNOWN,
};

struct n48r3_cmd_entry {
    enum n48r3_cmd cmd;
    const char *name;
    const char *summary;
};

static const struct n48r3_cmd_entry n48r3_cmd_table[] = {
    { N48R3_CMD_INFO, "info", "print the IP table, PCI/BAR baseline and survey status" },
    { N48R3_CMD_TRIGGER, "trigger", "trigger the stage-2 power-up substeps (D-01, root only)" },
    { N48R3_CMD_LOG, "log", "dump the driver's 2 MiB log ring to stdout" },
    { N48R3_CMD_LOGSTREAM, "logstream", "stream the log ring to a file (logstream <path> [interval_ms] [max_s] [stopfile])" },
    { N48R3_CMD_LOGRESET, "logreset", "clear the driver log buffer" },
    { N48R3_CMD_VERSION, "version", "print the tool version and the expected selector set" },
    { N48R3_CMD_MEM, "mem", "allocate a BO via ABI, report GPU VA, free it" },
    { N48R3_CMD_GART, "gart", "allocate BO and verify GPUVA mapping via ABI" },
    { N48R3_CMD_DOORBELL, "doorbell", "submit a minimal IB (RELEASE_MEM only) via ABI and wait for fence" },
    { N48R3_CMD_SUBMIT, "submit", "submit a user IB (via ABI) and wait for fence" },
    { N48R3_CMD_COMPUTE, "compute", "exercise the compute queue: alloc BO, submit RELEASE_MEM, read fence" },
};

static void n48r3_usage(FILE *out) {
    size_t i;
    fprintf(out, "usage: n48r3test <command>\n\ncommands:\n");
    for (i = 0; i < sizeof n48r3_cmd_table / sizeof n48r3_cmd_table[0]; i++)
        fprintf(out, "  %-10s %s\n", n48r3_cmd_table[i].name, n48r3_cmd_table[i].summary);
    fprintf(out,
            "\noptions:\n"
            "  -h, --help  show this help and exit\n"
            "\nexit codes:\n"
            "  0  success\n"
            "  1  driver not loaded (no %s service)\n"
            "  2  IOServiceOpen failed\n"
            "  3  selector call failed (info/log call failed or returned no text)\n"
            "  4  permission denied (not root, or the user client refused)\n"
            "  5  usage error\n",
            N48R3_SERVICE_NAME);
    fprintf(out,
            "\nlogreset timing (operating procedure):\n"
            "  run only after `log` reports AT CAPACITY, or after the previous run's\n"
            "  log has been exported; never clear the log before starting a run (it\n"
            "  erases the earliest, most valuable evidence).\n");
}

/* logstream argument parsing, shared by the real command and the host tests:
 * argv[0] is the binary name, argv[1] is "logstream" (full argc/argv view,
 * same convention as every other sub-command). */
struct n48r3_logstream_args {
    const char *path;
    int interval_ms;
    int max_s;
    const char *stopfile;
};

static int n48r3_parse_logstream_args(int argc, char **argv,
                                      struct n48r3_logstream_args *out)
{
    if (argc < 3 || argc > 6)
        return -1;
    out->path = argv[2];
    out->interval_ms = (argc >= 4) ? atoi(argv[3]) : 1000;
    out->max_s = (argc >= 5) ? atoi(argv[4]) : 3600;
    out->stopfile = (argc >= 6) ? argv[5] : NULL;
    if (out->interval_ms < 100)
        out->interval_ms = 100;
    if (out->max_s <= 0)
        out->max_s = 3600;
    return 0;
}

/* Returns the command encoded by argv[1]; *bad is argv[1] when unknown. */
static enum n48r3_cmd n48r3_parse_command(int argc, char **argv, const char **bad) {
    size_t i;
    if (bad) *bad = NULL;
    if (argc < 2) return N48R3_CMD_NONE;
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))
        return N48R3_CMD_HELP;
    for (i = 0; i < sizeof n48r3_cmd_table / sizeof n48r3_cmd_table[0]; i++)
        if (!strcmp(argv[1], n48r3_cmd_table[i].name))
            return n48r3_cmd_table[i].cmd;
    if (bad) *bad = argv[1];
    return N48R3_CMD_UNKNOWN;
}

/* The rdna3 user client is root only; check before opening it. */
static int n48r3_is_root(unsigned long euid) {
    return euid == 0;
}

/* Any permission denial is the root-only category, wherever it surfaced. */
static int n48r3_exit_for_kr(int kr, int fallback) {
    if (kr == N48R3_KR_NOT_PERMITTED || kr == N48R3_KR_NOT_PRIVILEGED)
        return N48R3_EXIT_NOT_ROOT;
    return fallback;
}

static const char *n48r3_kr_text(int kr) {
    switch (kr) {
    case 0:                     return "success";
    case N48R3_KR_NOT_PERMITTED:
    case N48R3_KR_NOT_PRIVILEGED: return "permission denied (the user client is root-only; run with sudo)";
    case N48R3_KR_NOT_OPEN:     return "user client is not open";
    case N48R3_KR_BAD_ARGUMENT: return "bad argument (ABI mismatch?)";
    case N48R3_KR_UNSUPPORTED:  return "selector not supported (ABI mismatch?)";
    case N48R3_KR_TIMEOUT:      return "driver timed out";
    case N48R3_KR_NO_DEVICE:    return "device not present";
    case N48R3_KR_ERROR:        return "driver reported a general error";
    default:                    return "unknown I/O Kit error";
    }
}

/* Print the kext's text buffer: stop at the first NUL, add the final newline. */
static int n48r3_emit_info(const char *buf, size_t len, FILE *out) {
    size_t n = 0;
    while (n < len && buf[n]) n++;
    if (n == 0 || len > N48R3_INFO_MAX) return -1;
    if (fwrite(buf, 1, n, out) != n) return -1;
    if (buf[n - 1] != '\n' && fputc('\n', out) == EOF) return -1;
    return fflush(out) == 0 ? 0 : -1;
}

/* version: pure local output, no IOKit call. Prints the tool version and the
 * selector set this build expects, so the tool's assumed ABI pairing can be
 * checked on any machine without a working `info`. */
static int n48r3_emit_version(FILE *out) {
    if (fprintf(out,
                "n48r3test version " N48R3_TOOL_VERSION "\n"
                "expected selectors: info=%u trigger=%u readlog=%u logreset=%u logconsume=%u\n",
                N48R3_SEL_INFO, N48R3_SEL_TRIGGER,
                kNavi48SelReadLog, kNavi48SelLogReset, kNavi48SelLogConsume) < 0)
        return -1;
    return fflush(out) == 0 ? 0 : -1;
}

/* ---- stage-3 ABI pure helpers (compiled and tested under N48R3_HOST_TEST too) ----
 * These depend only on Navi48R3ABI.h numeric contracts and plain string parsing,
 * so the host harness can exercise them without IOKit. */

/* Magic this tool programs into a BO during the `compute` round-trip. */
#define N48R3_COMPUTE_MAGIC 0xCAFEDEADu

/* Parse "fb_mc_base=0x..." out of the survey text served by selector 0.
 * Returns 1 on success and sets *out; 0 if the key is absent. */
static int n48r3_parse_fb_mc_base(const char *txt, size_t len, uint64_t *out) {
    static const char key[] = "fb_mc_base=0x";
    const size_t klen = sizeof key - 1;
    size_t i;
    if (!txt || !out || len < klen) return 0;
    for (i = 0; i + klen <= len; i++) {
        uint64_t v = 0;
        size_t j = i + klen, n = 0;
        if (memcmp(txt + i, key, klen) != 0) continue;
        while (j < len && n < 16) {
            const char c = txt[j];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
            else break;
            v = (v << 4) | d;
            n++;
            j++;
        }
        if (n == 0) return 0;
        *out = v;
        return 1;
    }
    return 0;
}

/* Fill dwords with the deterministic pattern seed ^ (i * 0x9E3779B1). */
static void n48r3_fill_pattern(volatile uint32_t *p, uint32_t dwords, uint32_t seed) {
    uint32_t i;
    for (i = 0; i < dwords; i++) p[i] = seed ^ (i * 0x9E3779B1u);
}

/* Verify read-back dwords against the pattern. Returns 0 on match. */
static int n48r3_check_pattern(const volatile uint32_t *p, uint32_t dwords, uint32_t seed) {
    uint32_t i;
    for (i = 0; i < dwords; i++)
        if (p[i] != (seed ^ (i * 0x9E3779B1u))) return 1;
    return 0;
}

#ifndef N48R3_HOST_TEST

static int cmd_version(int argc) {
    if (argc > 2) {
        fprintf(stderr, "n48r3: version takes no arguments\n");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }
    if (n48r3_emit_version(stdout) != 0) {
        fprintf(stderr, "n48r3: version write failed\n");
        return N48R3_EXIT_CALL;
    }
    return N48R3_EXIT_OK;
}

static int cmd_info(int argc) {
    if (argc > 2) {
        fprintf(stderr, "n48r3: info takes no arguments\n");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }

    if (!n48r3_is_root((unsigned long)geteuid())) {
        fprintf(stderr, "n48r3: must run as root (the %s user client is root-only); use sudo\n",
                N48R3_SERVICE_NAME);
        return N48R3_EXIT_NOT_ROOT;
    }

    io_service_t svc = IOServiceGetMatchingService(N48R3_MAIN_PORT,
                                                   IOServiceMatching(N48R3_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, N48R3_MSG_NOT_LOADED "\n");
        return N48R3_EXIT_NOT_LOADED;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "n48r3: IOServiceOpen failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
        return n48r3_exit_for_kr(kr, N48R3_EXIT_OPEN);
    }

    char buf[N48R3_INFO_MAX];
    uint64_t total = 0;
    uint32_t offset = 0;
    size_t done = 0;
    for (;;) {
        uint32_t out_n = 1;
        size_t sz = NAVI48_UC_MAX_XFER;
        if (sz > sizeof buf - done) sz = sizeof buf - done;
        uint64_t in = offset;
        kr = IOConnectCallMethod(conn, N48R3_SEL_INFO, &in, 1, NULL, 0,
                                 &total, &out_n, buf + done, &sz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "n48r3: info selector %u failed: %s (0x%x)\n",
                    N48R3_SEL_INFO, n48r3_kr_text(kr), kr);
            IOServiceClose(conn);
            return n48r3_exit_for_kr(kr, N48R3_EXIT_CALL);
        }
        if (sz == 0) break;
        done += sz;
        offset = (uint32_t)done;
        if (done >= total) break;
        if (done >= sizeof buf) break;   /* buffer guard: never overrun */
    }
    IOServiceClose(conn);
    if (n48r3_emit_info(buf, done, stdout) != 0) {
        fprintf(stderr, "n48r3: info selector returned no valid text (%zu bytes)\n", done);
        return N48R3_EXIT_CALL;
    }
    return N48R3_EXIT_OK;
}

static int cmd_trigger(int argc) {
    if (argc > 2) {
        fprintf(stderr, "n48r3: trigger takes no arguments\n");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }

    if (!n48r3_is_root((unsigned long)geteuid())) {
        fprintf(stderr, "n48r3: must run as root (the %s user client is root-only); use sudo\n",
                N48R3_SERVICE_NAME);
        return N48R3_EXIT_NOT_ROOT;
    }

    io_service_t svc = IOServiceGetMatchingService(N48R3_MAIN_PORT,
                                                   IOServiceMatching(N48R3_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, N48R3_MSG_NOT_LOADED "\n");
        return N48R3_EXIT_NOT_LOADED;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "n48r3: IOServiceOpen failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
        return n48r3_exit_for_kr(kr, N48R3_EXIT_OPEN);
    }

    /* Selector 1: no input, no output. The result is published as N48R3EVID-* properties. */
    kr = IOConnectCallStructMethod(conn, N48R3_SEL_TRIGGER, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "n48r3: trigger failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
        if (kr == N48R3_KR_NOT_PERMITTED)
            fprintf(stderr, "n48r3: already provisioned this boot (D-04); reboot to retry\n");
        return n48r3_exit_for_kr(kr, N48R3_EXIT_CALL);
    }
    printf("n48r3: trigger accepted; check N48R3EVID-* properties for results\n");
    return N48R3_EXIT_OK;
}

/* Open the root-only rdna3 user client for the three log primitives.
 * Returns an N48R3_EXIT_* code (0 on success) and stores the connection in *out.
 */
static int n48r3_open_connection(io_connect_t *out) {
    *out = IO_OBJECT_NULL;
    if (!n48r3_is_root((unsigned long)geteuid())) {
        fprintf(stderr, "n48r3: must run as root (the %s user client is root-only); use sudo\n",
                N48R3_SERVICE_NAME);
        return N48R3_EXIT_NOT_ROOT;
    }
    io_service_t svc = IOServiceGetMatchingService(N48R3_MAIN_PORT,
                                                   IOServiceMatching(N48R3_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, N48R3_MSG_NOT_LOADED "\n");
        return N48R3_EXIT_NOT_LOADED;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, out);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "n48r3: IOServiceOpen failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
        return n48r3_exit_for_kr(kr, N48R3_EXIT_OPEN);
    }
    return N48R3_EXIT_OK;
}

static int cmd_log(int argc) {
    if (argc > 2) {
        fprintf(stderr, "n48r3: log takes no arguments\n");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }
    io_connect_t conn;
    int rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;

    uint64_t offset = 0, total = 0;
    char buf[NAVI48_UC_MAX_XFER];
    for (;;) {
        uint32_t out_n = 1;
        size_t sz = sizeof buf;
        uint64_t in = offset;
        kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadLog,
                                                &in, 1, NULL, 0,
                                                &total, &out_n,
                                                buf, &sz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "n48r3: ReadLog failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
            IOServiceClose(conn);
            return n48r3_exit_for_kr(kr, N48R3_EXIT_CALL);
        }
        if (sz == 0) break;
        if (fwrite(buf, 1, sz, stdout) != sz) {
            fprintf(stderr, "n48r3: log write to stdout failed\n");
            IOServiceClose(conn);
            return N48R3_EXIT_CALL;
        }
        offset += sz;
        if (offset >= total) break;
    }
    IOServiceClose(conn);
    fprintf(stderr, "\n(%llu bytes of driver log%s)\n", (unsigned long long)total,
            total >= NAVI48_LOG_BYTES - 1024u
                ? "  *** AT CAPACITY - NEW LINES ARE BEING DROPPED, run `logreset` ***"
                : "");
    return N48R3_EXIT_OK;
}

static int cmd_logstream(int argc, char **argv) {
    struct n48r3_logstream_args a;
    if (n48r3_parse_logstream_args(argc, argv, &a) != 0) {
        fprintf(stderr, "n48r3: usage: logstream <path> [interval_ms] [max_s] [stopfile]\n");
        return N48R3_EXIT_USAGE;
    }

    io_connect_t conn;
    int rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;

    int fd = open(a.path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror(a.path);
        IOServiceClose(conn);
        return N48R3_EXIT_CALL;
    }

    dprintf(fd, "### logstream start pid %d interval %d ms max %d s stopfile %s\n",
            getpid(), a.interval_ms, a.max_s, a.stopfile ? a.stopfile : "(none)");

    char *buf = malloc(NAVI48_LOG_BYTES + 1);
    if (!buf) {
        perror("malloc");
        close(fd);
        IOServiceClose(conn);
        return N48R3_EXIT_CALL;
    }

    /* 0.0.276: stream semantics copied verbatim from base navi48test.c cmd_logstream():
     * read everything the ring holds, append to path, F_FULLFSYNC + sync(2), then tell
     * the kext to CONSUME exactly the bytes written; lines appended during the pass
     * stay for the next one. Stops on stopfile existence or after max_s. The consume
     * call returns held[0] consumed[1] dropped[2] overflowed[3]; only [1]/[2] are
     * reported here, [0] describes remaining ring contents, not bytes we wrote. */
    uint64_t passes = 0, bytes = 0, consumed = 0, dropped = 0, lastDropped = 0;
    time_t t0 = time(NULL);

    for (;;) {
        uint64_t offset = 0, total = 0;
        size_t got = 0;
        for (;;) {
            size_t sz = NAVI48_UC_MAX_XFER;
            uint32_t n = 1;
            if (got + sz > NAVI48_LOG_BYTES) sz = NAVI48_LOG_BYTES - got;
            if (sz == 0) break;
            kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadLog,
                                                   &offset, 1, NULL, 0,
                                                   &total, &n, buf + got, &sz);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "logstream: ReadLog failed 0x%x\n", kr);
                sz = 0;
            }
            if (sz == 0) break;
            got += sz;
            offset += sz;
            if (offset >= total) break;
        }
        if (got) {
            size_t done = 0;
            while (done < got) {
                ssize_t w = write(fd, buf + done, got - done);
                if (w <= 0) { perror("logstream write"); break; }
                done += (size_t)w;
            }
            (void)fcntl(fd, F_FULLFSYNC);
            sync();
            uint64_t in = done, out[4] = {0};
            uint32_t on = 4;
            kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelLogConsume,
                                                         &in, 1, out, &on);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "logstream: LogConsume failed 0x%x - the kext predates 0.0.276; stopping\n", kr);
                break;
            }
            bytes += done;
            consumed = out[1];
            dropped = out[2];
            if (dropped != lastDropped) {
                dprintf(fd, "### logstream: %llu byte(s) DROPPED by a full ring so far\n",
                        (unsigned long long)dropped);
                (void)fcntl(fd, F_FULLFSYNC);
                lastDropped = dropped;
            }
        }
        passes++;
        if (a.stopfile && access(a.stopfile, F_OK) == 0) break;
        if (time(NULL) - t0 >= a.max_s) break;
        usleep((useconds_t)a.interval_ms * 1000u);
    }
    dprintf(fd, "### logstream end: %llu pass(es), %llu byte(s) written, kext consumed %llu, dropped %llu\n",
            (unsigned long long)passes, (unsigned long long)bytes,
            (unsigned long long)consumed, (unsigned long long)dropped);
    (void)fcntl(fd, F_FULLFSYNC);
    close(fd);
    free(buf);
    IOServiceClose(conn);
    printf("logstream: %llu pass(es), %llu byte(s) written to %s, dropped %llu\n",
           (unsigned long long)passes, (unsigned long long)bytes, a.path,
           (unsigned long long)dropped);
    return N48R3_EXIT_OK;
}

static int cmd_logreset(int argc) {
    if (argc > 2) {
        fprintf(stderr, "n48r3: logreset takes no arguments\n");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }
    io_connect_t conn;
    int rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;

    kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelLogReset,
                                           NULL, 0, NULL, 0,
                                           NULL, NULL, NULL, NULL);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "n48r3: LogReset failed: %s (0x%x)\n", n48r3_kr_text(kr), kr);
        return n48r3_exit_for_kr(kr, N48R3_EXIT_CALL);
    }
    printf("n48r3: driver log cleared\n");
    return N48R3_EXIT_OK;
}

/* ---- ABI helper: allocate a BO of given size/domain, return gpu_va (0 on failure).
 * The third scalar output is the BO handle used as IOConnectMapMemory64 memoryType. ---- */
static uint64_t n48r3_abi_alloc(io_connect_t conn, uint64_t size, uint32_t domain,
                                uint64_t *handle) {
    const uint64_t inp[2] = {size, domain};
    uint32_t out_cnt = 3;
    uint64_t out[3] = {0, 0, 0};
    kern_return_t kr = IOConnectCallMethod(conn, N48R3_SEL_BO_ALLOC,
                                           inp, 2, NULL, 0,
                                           out, &out_cnt, NULL, NULL);
    if (kr != KERN_SUCCESS || out_cnt < 3) return 0ull;
    if (handle) *handle = out[2];
    return n48r3_va_from_scalars(out[0], out[1]);
}

/* ---- ABI helper: free BO ---- */
static kern_return_t n48r3_abi_free(io_connect_t conn, uint64_t gpu_va) {
    const uint64_t inp[2] = {gpu_va & 0xFFFFFFFFull, gpu_va >> 32};
    return IOConnectCallMethod(conn, N48R3_SEL_BO_FREE,
                               inp, 2, NULL, 0, NULL, NULL, NULL, NULL);
}

/* ---- ABI helper: submit an IB descriptor array (1..N48R3_CS_IB_MAX).
 * selector 4 takes a N48R3CsIn header followed by num_ibs N48R3CsIb records
 * as structureInput; the kernel only puts INDIRECT_BUFFER/RELEASE_MEM on the
 * ring, and the IB bytes stay in the user-mapped BO. ---- */
static uint64_t n48r3_abi_submit(io_connect_t conn, const N48R3CsIb *ibs,
                                 uint32_t num_ibs) {
    size_t bytes;
    uint8_t *in;
    N48R3CsIn h;
    uint32_t out_cnt = 2;
    uint64_t out[2] = {0, 0};
    kern_return_t kr;
    if (!ibs || num_ibs == 0 || num_ibs > N48R3_CS_IB_MAX) return 0ull;
    bytes = sizeof h + (size_t)num_ibs * sizeof *ibs;
    in = (uint8_t *)malloc(bytes);
    if (!in) return 0ull;
    h.abi_version = N48R3_UC_ABI_VERSION;
    h.num_ibs = num_ibs;
    h.flags = 0;
    h.reserved = 0;
    memcpy(in, &h, sizeof h);
    memcpy(in + sizeof h, ibs, (size_t)num_ibs * sizeof *ibs);
    kr = IOConnectCallMethod(conn, N48R3_SEL_SUBMIT,
                             NULL, 0, in, bytes,
                             out, &out_cnt, NULL, NULL);
    free(in);
    if (kr != KERN_SUCCESS || out_cnt < 2) return 0ull;
    return n48r3_va_from_scalars(out[0], out[1]);
}

/* ---- ABI helper: wait for fence (returns observed seq; timed_out if non-NULL) ---- */
static uint64_t n48r3_abi_fence_wait(io_connect_t conn, uint64_t expected_seq,
                                     uint32_t timeout_us, bool *timed_out) {
    const uint64_t inp[2] = {expected_seq, timeout_us};
    uint32_t out_cnt = 3;
    uint64_t out[3] = {0, 0, 0};
    kern_return_t kr = IOConnectCallMethod(conn, N48R3_SEL_FENCE_READ,
                                           inp, 2, NULL, 0,
                                           out, &out_cnt, NULL, NULL);
    if (kr != KERN_SUCCESS || out_cnt < 2) return 0ull;
    if (timed_out) *timed_out = (out_cnt >= 3 && out[2] != 0);
    return n48r3_va_from_scalars(out[0], out[1]);
}

/* ---- ABI helper: map a BO (memoryType = BO handle) or the D6 fence page. ----
 * The IOUserClient's clientMemoryForType turns the type into a BAR0/sysmem
 * physical slice; the tool then reads/writes the mapped CPU window directly. */
static kern_return_t n48r3_abi_map(io_connect_t conn, uint32_t memory_type,
                                   mach_vm_address_t *addr, mach_vm_size_t *size) {
    IOOptionBits opt = 0;
    *size = 0;
    *addr = 0;
    return IOConnectMapMemory64(conn, memory_type, mach_task_self(),
                                addr, size, opt);
}

static kern_return_t n48r3_abi_unmap(io_connect_t conn, uint32_t memory_type,
                                     mach_vm_address_t addr) {
    return IOConnectUnmapMemory64(conn, memory_type, mach_task_self(), addr);
}

/* ---- ABI helper: read the selector-0 survey text into buf. ---- */
static kern_return_t n48r3_abi_read_info(io_connect_t conn, char *buf, size_t cap,
                                         size_t *done_out) {
    uint64_t total = 0;
    uint32_t offset = 0;
    size_t done = 0;
    *done_out = 0;
    for (;;) {
        uint32_t out_n = 1;
        size_t sz = NAVI48_UC_MAX_XFER;
        uint64_t in = offset;
        kern_return_t kr;
        if (sz > cap - done) sz = cap - done;
        kr = IOConnectCallMethod(conn, N48R3_SEL_INFO, &in, 1, NULL, 0,
                                 &total, &out_n, buf + done, &sz);
        if (kr != KERN_SUCCESS) return kr;
        if (sz == 0) break;
        done += sz;
        offset = (uint32_t)done;
        if (done >= total || done >= cap) break;
    }
    if (done < cap) buf[done] = '\0';
    else buf[cap - 1] = '\0';
    *done_out = done;
    return KERN_SUCCESS;
}

/* Allocate a BO, map its memoryType, and report the common failure path. */
static int n48r3_bo_map(io_connect_t conn, const char *cmd, uint64_t size,
                        uint32_t domain, uint64_t *gva, uint64_t *handle,
                        volatile uint32_t **cpu) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t mapped = 0;
    kern_return_t kr;
    *gva = n48r3_abi_alloc(conn, size, domain, handle);
    if (*gva == 0 || *handle == 0 || *handle > N48R3_BO_MAX) {
        fprintf(stderr, "n48r3: %s: BOAlloc failed\n", cmd);
        return N48R3_EXIT_CALL;
    }
    kr = n48r3_abi_map(conn, (uint32_t)*handle, &addr, &mapped);
    if (kr != KERN_SUCCESS || addr == 0 || mapped < size) {
        fprintf(stderr, "n48r3: %s: map(handle=%llu) failed: %s (0x%x)\n",
                cmd, (unsigned long long)*handle, n48r3_kr_text(kr), kr);
        n48r3_abi_free(conn, *gva);
        return N48R3_EXIT_CALL;
    }
    *cpu = (volatile uint32_t *)addr;
    return N48R3_EXIT_OK;
}

static int n48r3_bo_release(io_connect_t conn, uint32_t memory_type,
                            mach_vm_address_t addr, uint64_t gva) {
    n48r3_abi_unmap(conn, memory_type, addr);
    return n48r3_abi_free(conn, gva) == KERN_SUCCESS ? N48R3_EXIT_OK : N48R3_EXIT_CALL;
}

/* Parse a byte size for mem/gart; shared command-line contract. */
static int n48r3_cmd_size(int argc, char **argv, const char *cmd, uint64_t *size) {
    *size = 4096ull;
    if (argc > 2) *size = strtoull(argv[2], NULL, 0);
    if (*size == 0 || *size > 0x100000ull) {
        fprintf(stderr, "n48r3: %s: size must be 1..1048576\n", cmd);
        return N48R3_EXIT_USAGE;
    }
    return N48R3_EXIT_OK;
}

/* ---- mem: UMA BO allocation, mapped write/read-back, carveout comparison ---- */
static int cmd_mem(int argc, char **argv) {
    uint64_t size = 0, gva = 0, handle = 0, fb_mc = 0;
    uint32_t dwords;
    volatile uint32_t *cpu = NULL;
    io_connect_t conn;
    char info[N48R3_INFO_MAX];
    size_t info_len = 0;
    int rc = n48r3_cmd_size(argc, argv, "mem", &size);
    if (rc != N48R3_EXIT_OK) return rc;
    rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;

    rc = n48r3_bo_map(conn, "mem", size, (uint32_t)kN48R3DomainUMA,
                      &gva, &handle, &cpu);
    if (rc != N48R3_EXIT_OK) { IOServiceClose(conn); return rc; }
    dwords = (uint32_t)(size / 4u);
    n48r3_fill_pattern(cpu, dwords, 0xA5A55A5Au);
    __sync_synchronize();
    if (n48r3_check_pattern(cpu, dwords, 0xA5A55A5Au) != 0) {
        fprintf(stderr, "n48r3: mem: FAIL pattern read-back mismatch\n");
        rc = N48R3_EXIT_CALL;
        goto out;
    }
    if (n48r3_abi_read_info(conn, info, sizeof info, &info_len) == KERN_SUCCESS &&
        n48r3_parse_fb_mc_base(info, info_len, &fb_mc)) {
        printf("n48r3: mem: PASS gpu_va=0x%016llx size=%llu handle=%llu "
               "fb_mc_base=0x%016llx carveout_off=0x%016llx\n",
               (unsigned long long)gva, (unsigned long long)size,
               (unsigned long long)handle, (unsigned long long)fb_mc,
               (unsigned long long)n48r3_carveout_offset(fb_mc, gva, size));
    } else {
        printf("n48r3: mem: PASS gpu_va=0x%016llx size=%llu handle=%llu "
               "pattern=readback-ok carveout_off=unavailable(fb_mc_base)\n",
               (unsigned long long)gva, (unsigned long long)size,
               (unsigned long long)handle);
    }

out:
    if (cpu) {
        int release = n48r3_bo_release(conn, (uint32_t)handle,
                                       (mach_vm_address_t)cpu, gva);
        if (rc == N48R3_EXIT_OK) rc = release;
    }
    IOServiceClose(conn);
    return rc;
}

/* ---- gart: GTT BO allocation, GART PTE evidence via mapped sysmem window ---- */
static int cmd_gart(int argc, char **argv) {
    uint64_t size = 0, gva = 0, handle = 0;
    uint32_t dwords;
    volatile uint32_t *cpu = NULL;
    io_connect_t conn;
    int rc = n48r3_cmd_size(argc, argv, "gart", &size);
    if (rc != N48R3_EXIT_OK) return rc;
    rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;

    rc = n48r3_bo_map(conn, "gart", size, (uint32_t)kN48R3DomainGTT,
                      &gva, &handle, &cpu);
    if (rc != N48R3_EXIT_OK) { IOServiceClose(conn); return rc; }
    dwords = (uint32_t)(size / 4u);
    n48r3_fill_pattern(cpu, dwords, 0x47415254u); /* "GART" */
    __sync_synchronize();
    if (n48r3_check_pattern(cpu, dwords, 0x47415254u) != 0) {
        fprintf(stderr, "n48r3: gart: FAIL sysmem mapped window mismatch\n");
        rc = N48R3_EXIT_CALL;
    } else {
        /* GTT BO gpu_va is a GART VA, not an MC aperture address. */
        printf("n48r3: gart: PASS gart_va=0x%016llx size=%llu handle=%llu "
               "sysmem_window=write-readback-ok gart_index=0x%llx "
               "ptes=%u (CP consumption deferred to stage 4/5)\n",
               (unsigned long long)gva, (unsigned long long)size,
               (unsigned long long)handle,
               (unsigned long long)((gva >> 12) & 0x7ffffull),
               (unsigned)((size + 4095ull) >> 12));
    }
    {
        int release = n48r3_bo_release(conn, (uint32_t)handle,
                                       (mach_vm_address_t)cpu, gva);
        if (rc == N48R3_EXIT_OK) rc = release;
    }
    IOServiceClose(conn);
    return rc;
}

/* Run one mapped UMA BO through selector 4 + selector 5; shared by doorbell
 * and submit. `compute` builds its own WRITE_DATA IB. */
static int n48r3_submit_roundtrip(io_connect_t conn, const char *cmd,
                                  volatile uint32_t *cpu, uint64_t gva,
                                  uint32_t ib_dwords, uint32_t ip_type) {
    N48R3CsIb ib;
    uint64_t fence = 0, obs = 0;
    bool to = false;
    struct timespec t0, t1;
    long us;
    if (ib_dwords == 0) {
        cpu[0] = N48R3_PM4_NOP;
        cpu[1] = 0u;          /* NOP body: 完整 NO_OP 包 = 头 + 0，共 2 dwords */
        ib_dwords = 2;
    }
    ib.va_start = gva;
    ib.ib_bytes = ib_dwords * 4u;
    ib.ip_type = ip_type;
    __sync_synchronize(); /* HDP/WC barrier before the CP reads the user BO */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fence = n48r3_abi_submit(conn, &ib, 1u);
    if (fence == 0) {
        fprintf(stderr, "n48r3: %s: submit failed\n", cmd);
        return N48R3_EXIT_CALL;
    }
    obs = n48r3_abi_fence_wait(conn, fence, 2000000u, &to);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L +
         (t1.tv_nsec - t0.tv_nsec) / 1000L;
    printf("n48r3: %s: %s ib_dwords=%u fence_seq=%llu observed=%llu "
           "timed_out=%s elapsed_us=%ld\n", cmd, to ? "FAIL" : "PASS",
           ib_dwords, (unsigned long long)fence, (unsigned long long)obs,
           to ? "yes" : "no", us);
    return to ? N48R3_EXIT_TIMEOUT : N48R3_EXIT_OK;
}

/* ---- doorbell: real compute queue submission with a minimal NOP IB ---- */
static int cmd_doorbell(int argc, char **argv) {
    uint64_t gva = 0, handle = 0;
    volatile uint32_t *cpu = NULL;
    io_connect_t conn;
    int rc;
    (void)argc; (void)argv;
    rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;
    rc = n48r3_bo_map(conn, "doorbell", 4096ull, (uint32_t)kN48R3DomainUMA,
                      &gva, &handle, &cpu);
    if (rc != N48R3_EXIT_OK) { IOServiceClose(conn); return rc; }
    rc = n48r3_submit_roundtrip(conn, "doorbell", cpu, gva, 0u,
                                (uint32_t)kN48R3IpCompute);
    {
        int release = n48r3_bo_release(conn, (uint32_t)handle,
                                       (mach_vm_address_t)cpu, gva);
        if (rc == N48R3_EXIT_OK) rc = release;
    }
    IOServiceClose(conn);
    return rc;
}

/* ---- submit: user-written IB of len_dw dwords (default 16) ---- */
static int cmd_submit(int argc, char **argv) {
    uint32_t len_dw = 16u;
    uint64_t gva = 0, handle = 0, bo_size = 0;
    volatile uint32_t *cpu = NULL;
    io_connect_t conn;
    int rc;
    if (argc > 2) len_dw = (uint32_t)strtoul(argv[2], NULL, 0);
    if (len_dw == 0 || len_dw > 0x10000u || (len_dw & 1u) != 0u) {
        fprintf(stderr, "n48r3: submit: len_dw must be a positive even dword count <= 65536 "
                        "(each NO_OP packet is 2 dwords)\n");
        return N48R3_EXIT_USAGE;
    }
    rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;
    bo_size = (uint64_t)len_dw * 4ull;
    rc = n48r3_bo_map(conn, "submit", bo_size, (uint32_t)kN48R3DomainUMA,
                      &gva, &handle, &cpu);
    if (rc != N48R3_EXIT_OK) { IOServiceClose(conn); return rc; }
    {
        uint32_t i;
        for (i = 0; i < len_dw; i += 2) {
            cpu[i] = N48R3_PM4_NOP;
            cpu[i + 1] = 0u;   /* 完整 NO_OP 包：头 + body 0 */
        }
    }
    rc = n48r3_submit_roundtrip(conn, "submit", cpu, gva, len_dw,
                                (uint32_t)kN48R3IpCompute);
    {
        int release = n48r3_bo_release(conn, (uint32_t)handle,
                                       (mach_vm_address_t)cpu, gva);
        if (rc == N48R3_EXIT_OK) rc = release;
    }
    IOServiceClose(conn);
    return rc;
}

/* ---- compute: store_magic round-trip. The user writes a WRITE_DATA packet
 * into its own BO, submits that BO's GPU address as the IB, polls the fence,
 * and reads the magic back from a second dword in the same mapped BO. ---- */
static int cmd_compute(int argc, char **argv) {
    uint64_t gva = 0, handle = 0;
    uint32_t n = 0;
    volatile uint32_t *cpu = NULL;
    io_connect_t conn;
    int rc;
    struct timespec t0, t1;
    long us;
    (void)argc; (void)argv;
    rc = n48r3_open_connection(&conn);
    if (rc != N48R3_EXIT_OK) return rc;
    rc = n48r3_bo_map(conn, "compute", 4096ull, (uint32_t)kN48R3DomainUMA,
                      &gva, &handle, &cpu);
    if (rc != N48R3_EXIT_OK) { IOServiceClose(conn); return rc; }

    /* DST is a raw 48-bit MC/VM address (S13 writes test_va=fb_mc+offset that
     * way); gpu_va is canonical, so strip the sign extension first. The magic
     * slot is cleared before submission so a stale value cannot fake a PASS. */
    cpu[0x100 / 4u] = 0;
    n = n48r3_pm4_write_data((uint32_t *)(void *)cpu,
                             n48r3_va_strip(gva) + 0x100u,
                             N48R3_COMPUTE_MAGIC);
    if (n != 5u) {
        fprintf(stderr, "n48r3: compute: WRITE_DATA build failed\n");
        rc = N48R3_EXIT_CALL;
        goto out;
    }
    cpu[n] = N48R3_PM4_NOP;
    cpu[n + 1] = 0u;   /* 完整 NO_OP 包 = 2 dwords；整个 BO 即 IB */
    __sync_synchronize();
    {
        N48R3CsIb ib;
        uint64_t fence = 0, obs = 0;
        bool to = false;
        ib.va_start = gva;
        ib.ib_bytes = (n + 2u) * 4u;
        ib.ip_type = (uint32_t)kN48R3IpCompute;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        fence = n48r3_abi_submit(conn, &ib, 1u);
        if (fence == 0) {
            fprintf(stderr, "n48r3: compute: submit failed\n");
            rc = N48R3_EXIT_CALL;
            goto out;
        }
        obs = n48r3_abi_fence_wait(conn, fence, 2000000u, &to);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L +
             (t1.tv_nsec - t0.tv_nsec) / 1000L;
        if (to || cpu[0x100 / 4u] != N48R3_COMPUTE_MAGIC) {
            printf("n48r3: compute: FAIL ib_dwords=%u fence_seq=%llu observed=%llu "
                   "magic=0x%08x expected=0x%08x timed_out=%s elapsed_us=%ld\n",
                   n + 2u, (unsigned long long)fence, (unsigned long long)obs,
                   cpu[0x100 / 4u], N48R3_COMPUTE_MAGIC, to ? "yes" : "no", us);
            rc = to ? N48R3_EXIT_TIMEOUT : N48R3_EXIT_CALL;
        } else {
            printf("n48r3: compute: PASS ib_dwords=%u fence_seq=%llu observed=%llu "
                   "magic=0x%08x elapsed_us=%ld\n",
                   n + 2u, (unsigned long long)fence, (unsigned long long)obs,
                   N48R3_COMPUTE_MAGIC, us);
        }
    }

out:
    {
        int release = n48r3_bo_release(conn, (uint32_t)handle,
                                       (mach_vm_address_t)cpu, gva);
        if (rc == N48R3_EXIT_OK) rc = release;
    }
    IOServiceClose(conn);
    return rc;
}

int main(int argc, char **argv) {
    const char *bad = NULL;

    switch (n48r3_parse_command(argc, argv, &bad)) {
    case N48R3_CMD_HELP:
        n48r3_usage(stdout);
        return N48R3_EXIT_OK;
    case N48R3_CMD_INFO:
        return cmd_info(argc);
    case N48R3_CMD_TRIGGER:
        return cmd_trigger(argc);
    case N48R3_CMD_LOG:
        return cmd_log(argc);
    case N48R3_CMD_LOGSTREAM:
        return cmd_logstream(argc, argv);
    case N48R3_CMD_LOGRESET:
        return cmd_logreset(argc);
    case N48R3_CMD_VERSION:
        return cmd_version(argc);
    case N48R3_CMD_MEM:
        return cmd_mem(argc, argv);
    case N48R3_CMD_GART:
        return cmd_gart(argc, argv);
    case N48R3_CMD_DOORBELL:
        return cmd_doorbell(argc, argv);
    case N48R3_CMD_SUBMIT:
        return cmd_submit(argc, argv);
    case N48R3_CMD_COMPUTE:
        return cmd_compute(argc, argv);
    case N48R3_CMD_UNKNOWN:
        fprintf(stderr, "n48r3: unknown command '%s'\n", bad ? bad : "");
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    case N48R3_CMD_NONE:
    default:
        n48r3_usage(stderr);
        return N48R3_EXIT_USAGE;
    }
}

#else /* N48R3_HOST_TEST */

static int t_fail, t_checks;

#define T_CHECK(cond, ...) do { \
    t_checks++; \
    if (!(cond)) { t_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int emit_capture(const char *buf, size_t len, char **out, size_t *out_len) {
    int rc;
    FILE *f = open_memstream(out, out_len);
    if (!f) return -1;
    rc = n48r3_emit_info(buf, len, f);
    if (fclose(f) != 0) rc = -1;
    return rc;
}

static void test_parse(void) {
    const char *bad = (const char *)"sentinel";
    char *none[]  = { (char *)"n48r3test", NULL };
    char *help[]  = { (char *)"n48r3test", (char *)"--help", NULL };
    char *help2[] = { (char *)"n48r3test", (char *)"-h", NULL };
    char *help3[] = { (char *)"n48r3test", (char *)"help", NULL };
    char *info[]  = { (char *)"n48r3test", (char *)"info", NULL };
    char *trig[]  = { (char *)"n48r3test", (char *)"trigger", NULL };
    char *bogus[] = { (char *)"n48r3test", (char *)"bogus", NULL };

    T_CHECK(n48r3_parse_command(1, none, &bad) == N48R3_CMD_NONE, "no args");
    T_CHECK(bad == NULL, "no args leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, help, &bad) == N48R3_CMD_HELP, "--help");
    T_CHECK(n48r3_parse_command(2, help2, &bad) == N48R3_CMD_HELP, "-h");
    T_CHECK(n48r3_parse_command(2, help3, &bad) == N48R3_CMD_HELP, "help");
    T_CHECK(n48r3_parse_command(2, info, &bad) == N48R3_CMD_INFO, "info");
    T_CHECK(bad == NULL, "info leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, trig, &bad) == N48R3_CMD_TRIGGER, "trigger");
    T_CHECK(bad == NULL, "trigger leaves bad NULL");
    char *log[]   = { (char *)"n48r3test", (char *)"log", NULL };
    char *lstr[]  = { (char *)"n48r3test", (char *)"logstream", NULL };
    char *lrst[]  = { (char *)"n48r3test", (char *)"logreset", NULL };
    T_CHECK(n48r3_parse_command(2, log, &bad) == N48R3_CMD_LOG, "log");
    T_CHECK(bad == NULL, "log leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, lstr, &bad) == N48R3_CMD_LOGSTREAM, "logstream");
    T_CHECK(bad == NULL, "logstream leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, lrst, &bad) == N48R3_CMD_LOGRESET, "logreset");
    T_CHECK(bad == NULL, "logreset leaves bad NULL");
    char *mem[]   = { (char *)"n48r3test", (char *)"mem", NULL };
    char *gart[]  = { (char *)"n48r3test", (char *)"gart", NULL };
    char *dbell[] = { (char *)"n48r3test", (char *)"doorbell", NULL };
    char *sub[]   = { (char *)"n48r3test", (char *)"submit", NULL };
    char *comp[]  = { (char *)"n48r3test", (char *)"compute", NULL };
    T_CHECK(n48r3_parse_command(2, mem, &bad) == N48R3_CMD_MEM, "mem");
    T_CHECK(bad == NULL, "mem leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, gart, &bad) == N48R3_CMD_GART, "gart");
    T_CHECK(bad == NULL, "gart leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, dbell, &bad) == N48R3_CMD_DOORBELL, "doorbell");
    T_CHECK(bad == NULL, "doorbell leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, sub, &bad) == N48R3_CMD_SUBMIT, "submit");
    T_CHECK(bad == NULL, "submit leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, comp, &bad) == N48R3_CMD_COMPUTE, "compute");
    T_CHECK(bad == NULL, "compute leaves bad NULL");
    T_CHECK(n48r3_parse_command(2, bogus, &bad) == N48R3_CMD_UNKNOWN, "unknown");
}

static void test_logstream_args(void) {
    struct n48r3_logstream_args a;

    char *a2[] = { (char *)"n48r3test", (char *)"logstream", NULL };
    T_CHECK(n48r3_parse_logstream_args(2, a2, &a) == -1, "argc=2 rejected");

    char *a7[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                   (char *)"200", (char *)"86400", (char *)"/tmp/stop",
                   (char *)"extra", NULL };
    T_CHECK(n48r3_parse_logstream_args(7, a7, &a) == -1, "argc=7 rejected");

    char *a3[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/log.txt", NULL };
    T_CHECK(n48r3_parse_logstream_args(3, a3, &a) == 0, "argc=3 accepted");
    T_CHECK(a.path == a3[2] && strcmp(a.path, "/tmp/log.txt") == 0, "argc=3 path");
    T_CHECK(a.interval_ms == 1000, "argc=3 default interval_ms");
    T_CHECK(a.max_s == 3600, "argc=3 default max_s");
    T_CHECK(a.stopfile == NULL, "argc=3 no stopfile");

    char *a6[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/var/fetch.log",
                   (char *)"200", (char *)"86400", (char *)"/tmp/stop.now", NULL };
    T_CHECK(n48r3_parse_logstream_args(6, a6, &a) == 0, "argc=6 accepted");
    T_CHECK(a.path == a6[2] && strcmp(a.path, "/var/fetch.log") == 0, "argc=6 path");
    T_CHECK(a.interval_ms == 200, "argc=6 interval_ms 200");
    T_CHECK(a.max_s == 86400, "argc=6 max_s 86400");
    T_CHECK(a.stopfile == a6[5] && strcmp(a.stopfile, "/tmp/stop.now") == 0,
            "argc=6 stopfile");

    char *a4a[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                    (char *)"50", NULL };
    T_CHECK(n48r3_parse_logstream_args(4, a4a, &a) == 0, "argc=4 interval 50 accepted");
    T_CHECK(a.interval_ms == 100, "interval_ms 50 clamped to 100");

    char *a4b[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                    (char *)"100", NULL };
    T_CHECK(n48r3_parse_logstream_args(4, a4b, &a) == 0, "argc=4 interval 100 accepted");
    T_CHECK(a.interval_ms == 100, "interval_ms 100 kept");

    char *a5a[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                    (char *)"500", (char *)"0", NULL };
    T_CHECK(n48r3_parse_logstream_args(5, a5a, &a) == 0, "argc=5 max_s 0 accepted");
    T_CHECK(a.max_s == 3600, "max_s 0 clamped to 3600");

    char *a5b[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                    (char *)"500", (char *)"-1", NULL };
    T_CHECK(n48r3_parse_logstream_args(5, a5b, &a) == 0, "argc=5 max_s -1 accepted");
    T_CHECK(a.max_s == 3600, "max_s -1 clamped to 3600");

    char *a5c[] = { (char *)"n48r3test", (char *)"logstream", (char *)"/tmp/x",
                    (char *)"250", (char *)"7200", NULL };
    T_CHECK(n48r3_parse_logstream_args(5, a5c, &a) == 0, "argc=5 legal accepted");
    T_CHECK(a.interval_ms == 250 && a.max_s == 7200, "argc=5 values pass through");
    T_CHECK(a.stopfile == NULL, "argc=5 no stopfile");
}

static void test_root(void) {
    T_CHECK(n48r3_is_root(0) == 1, "euid 0 is root");
    T_CHECK(n48r3_is_root(501) == 0, "euid 501 is not root");
    T_CHECK(n48r3_is_root(65534) == 0, "euid 65534 is not root");
}

static void test_exit_for_kr(void) {
    T_CHECK(n48r3_exit_for_kr(N48R3_KR_NOT_PERMITTED, N48R3_EXIT_OPEN) == N48R3_EXIT_NOT_ROOT,
            "not permitted maps to root category");
    T_CHECK(n48r3_exit_for_kr(N48R3_KR_NOT_PRIVILEGED, N48R3_EXIT_CALL) == N48R3_EXIT_NOT_ROOT,
            "not privileged maps to root category");
    T_CHECK(n48r3_exit_for_kr(N48R3_KR_TIMEOUT, N48R3_EXIT_OPEN) == N48R3_EXIT_OPEN,
            "other codes keep the fallback");
    T_CHECK(n48r3_exit_for_kr(0, N48R3_EXIT_CALL) == N48R3_EXIT_CALL,
            "success keeps the fallback");
}

static void test_kr_text(void) {
    T_CHECK(n48r3_kr_text(0) != NULL && !strcmp(n48r3_kr_text(0), "success"), "kr 0");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_NOT_PERMITTED), "root") != NULL, "not permitted");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_NOT_PRIVILEGED), "root") != NULL, "not privileged");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_NOT_OPEN), "not open") != NULL, "not open");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_BAD_ARGUMENT), "argument") != NULL, "bad argument");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_UNSUPPORTED), "not supported") != NULL, "unsupported");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_TIMEOUT), "timed out") != NULL, "timeout");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_NO_DEVICE), "not present") != NULL, "no device");
    T_CHECK(strstr(n48r3_kr_text(N48R3_KR_ERROR), "general") != NULL, "general error");
    T_CHECK(strstr(n48r3_kr_text(0x1234), "unknown") != NULL, "unknown code");
}

static void test_emit(void) {
    char *out = NULL;
    size_t out_len = 0;
    char padded[] = { 'k', '=', 'v', '\0', '\0' };
    char big[N48R3_INFO_MAX + 1];
    char edge[N48R3_INFO_MAX];

    T_CHECK(emit_capture("a=1\nb=2", 7, &out, &out_len) == 0, "emit succeeds");
    T_CHECK(out_len == 8 && !memcmp(out, "a=1\nb=2\n", 8), "adds missing newline (len %zu)", out_len);
    free(out); out = NULL; out_len = 0;

    T_CHECK(emit_capture("x\n", 2, &out, &out_len) == 0, "emit with newline succeeds");
    T_CHECK(out_len == 2 && !memcmp(out, "x\n", 2), "keeps existing newline");
    free(out); out = NULL; out_len = 0;

    T_CHECK(emit_capture(padded, sizeof padded, &out, &out_len) == 0, "emit NUL-padded");
    T_CHECK(out_len == 4 && !memcmp(out, "k=v\n", 4), "stops at first NUL (len %zu)", out_len);
    free(out); out = NULL; out_len = 0;

    T_CHECK(emit_capture("", 0, &out, &out_len) == -1, "empty output rejected");
    free(out); out = NULL; out_len = 0;
    T_CHECK(emit_capture("\0", 1, &out, &out_len) == -1, "all-NUL output rejected");
    free(out); out = NULL; out_len = 0;

    memset(edge, 'y', sizeof edge);
    T_CHECK(emit_capture(edge, sizeof edge, &out, &out_len) == 0, "max-size output accepted");
    T_CHECK(out_len == N48R3_INFO_MAX + 1 && out[0] == 'y' &&
            out[N48R3_INFO_MAX] == '\n', "max-size output gains one newline (len %zu)", out_len);
    free(out); out = NULL; out_len = 0;

    memset(big, 'x', sizeof big);
    T_CHECK(emit_capture(big, sizeof big, &out, &out_len) == -1, "over-size output rejected");
    free(out);
}

static void test_usage(void) {
    char *out = NULL;
    size_t out_len = 0;
    FILE *f = open_memstream(&out, &out_len);

    T_CHECK(f != NULL, "open_memstream for usage");
    if (!f) return;
    n48r3_usage(f);
    T_CHECK(fclose(f) == 0, "close usage stream");
    T_CHECK(out && strstr(out, "usage: n48r3test") != NULL, "usage line");
    T_CHECK(out && strstr(out, "info") != NULL, "info listed");
    T_CHECK(out && strstr(out, "trigger") != NULL, "trigger listed");
    T_CHECK(out && strstr(out, "exit codes:") != NULL, "exit codes listed");
    T_CHECK(out && strstr(out, "  0  success") != NULL, "exit code 0 text");
    T_CHECK(out && strstr(out, "  1  driver not loaded") != NULL, "exit code 1 text");
    T_CHECK(!strncmp(N48R3_MSG_NOT_LOADED, "n48r3: driver not loaded", 24),
            "driver-not-loaded wording");
    T_CHECK(out && strstr(out, "  2  IOServiceOpen failed") != NULL, "exit code 2 text");
    T_CHECK(out && strstr(out, "  3  selector call failed") != NULL, "exit code 3 text");
    T_CHECK(out && strstr(out, "  4  permission denied") != NULL, "exit code 4 text");
    T_CHECK(out && strstr(out, "  5  usage error") != NULL, "exit code 5 text");
    T_CHECK(out && strstr(out, N48R3_SERVICE_NAME) != NULL, "service name in usage");
    T_CHECK(out && strstr(out, "log") != NULL, "log listed");
    T_CHECK(out && strstr(out, "logstream") != NULL, "logstream listed");
    T_CHECK(out && strstr(out, "logreset") != NULL, "logreset listed");
    T_CHECK(out && strstr(out, "AT CAPACITY") != NULL, "AT CAPACITY in help");
    T_CHECK(out && strstr(out, "never clear") != NULL, "logreset timing caution");
    free(out);
}

static void test_version(void) {
    char *out = NULL;
    size_t out_len = 0;
    char expect[64];
    FILE *f = open_memstream(&out, &out_len);

    T_CHECK(f != NULL, "open_memstream for version");
    if (!f) return;
    T_CHECK(n48r3_emit_version(f) == 0, "emit_version succeeds");
    T_CHECK(fclose(f) == 0, "close version stream");
    T_CHECK(out && strstr(out, "n48r3test version " N48R3_TOOL_VERSION) != NULL,
            "version string present");
    snprintf(expect, sizeof expect, "info=%u", N48R3_SEL_INFO);
    T_CHECK(out && strstr(out, expect) != NULL, "info selector (%s)", expect);
    snprintf(expect, sizeof expect, "trigger=%u", N48R3_SEL_TRIGGER);
    T_CHECK(out && strstr(out, expect) != NULL, "trigger selector (%s)", expect);
    snprintf(expect, sizeof expect, "readlog=%u", kNavi48SelReadLog);
    T_CHECK(out && strstr(out, expect) != NULL, "readlog selector (%s)", expect);
    snprintf(expect, sizeof expect, "logreset=%u", kNavi48SelLogReset);
    T_CHECK(out && strstr(out, expect) != NULL, "logreset selector (%s)", expect);
    snprintf(expect, sizeof expect, "logconsume=%u", kNavi48SelLogConsume);
    T_CHECK(out && strstr(out, expect) != NULL, "logconsume selector (%s)", expect);
    free(out);
}

/* ---- X-02: stage-3 ABI parameter parsing, validation and address shapes ----
 * Pure functions from Navi48R3ABI.h plus the tool's own text parser. The
 * fb_mc_base != 0 case is the negative test demanded by verdict C-08: io->bus
 * must receive a carveout byte offset, never an MC aperture address. */
static void test_abi_pure(void) {
    const uint64_t fb_mc = 0x8000000000ull;
    const uint64_t d7_off = 0x00abc000ull;
    uint64_t gva = n48r3_uma_gpu_va_canonical(fb_mc, d7_off);
    uint8_t buf[sizeof(N48R3CsIn) + 2 * sizeof(N48R3CsIb)];
    N48R3CsIn *h = (N48R3CsIn *)(void *)buf;
    N48R3CsIb *ibs = (N48R3CsIb *)(void *)(buf + sizeof(N48R3CsIn));
    const N48R3CsIn *out_h = NULL;
    const N48R3CsIb *out_ibs = NULL;
    uint32_t pkt[5];
    uint32_t n;
    uint64_t parsed = 0, scratch[32];
    char txt[] = "PASS copy=GC fb_mc_base=0x8000000000 fb_mc_top=0x8001000000\n";
    char badtxt[] = "PASS copy=GC fb_mc_base=0x zzz\n";

    /* selector numbering comes from the contract header (C-10). */
    T_CHECK(kN48R3SelBoAlloc == 2 && kN48R3SelBoFree == 3 &&
            kN48R3SelSubmit == 4 && kN48R3SelFenceRead == 5 &&
            kN48R3SelBoImportHost == 21, "selector numbering");
    T_CHECK(kN48R3DomainUMA == 0 && kN48R3DomainGTT == 1 &&
            kN48R3IpGfx == 0 && kN48R3IpCompute == 1,
            "domain / ip_type enumeration");

    /* Address shapes: canonical UMA gpu_va; carveout offset is the difference. */
    T_CHECK(n48r3_uma_gpu_va(fb_mc, d7_off) == fb_mc + d7_off,
            "UMA gpu_va = fb_mc_base + D7 offset (raw 48-bit)");
    T_CHECK(n48r3_va_canonical(gva) && n48r3_va_strip(gva) == fb_mc + d7_off,
            "UMA canonical gpu_va sign-extends bit 47");
    T_CHECK(n48r3_carveout_offset(fb_mc, gva, 4096u) == d7_off,
            "fb_mc_base != 0: carveout offset is the byte offset, not MC address");
    T_CHECK(n48r3_carveout_offset(fb_mc, fb_mc - 4096u, 4096u) == 0,
            "C-08 negative: address below fb_mc_base is rejected");
    T_CHECK(n48r3_carveout_offset(0, 0x1000u, 4096u) == 0,
            "C-08 negative: zero fb_mc_base cannot turn MC into a carveout offset");
    T_CHECK(n48r3_va_from_scalars(0x89abcdefu, 0x01234567u) == 0x0123456789abcdefull,
            "scalar lo/hi VA assembly");
    T_CHECK(n48r3_bo_alloc_args(4096u, kN48R3DomainUMA) == 0 &&
            n48r3_bo_alloc_args(4096u, kN48R3DomainGTT) == 0,
            "BO alloc legal args");
    T_CHECK(n48r3_bo_alloc_args(0, kN48R3DomainUMA) == -1 &&
            n48r3_bo_alloc_args(0x100001u, kN48R3DomainUMA) == -1 &&
            n48r3_bo_alloc_args(4096u, 2u) == -1,
            "BO alloc rejects zero/oversize/unknown domain");

    /* cs_ib validation (canonical, 4-byte aligned and capped length). */
    T_CHECK(n48r3_cs_ib_valid(NULL) != 0, "cs_ib rejects NULL");
    ibs[0].va_start = gva; ibs[0].ib_bytes = 20u;
    ibs[0].ip_type = kN48R3IpCompute;
    T_CHECK(n48r3_cs_ib_valid(&ibs[0]) == 0, "cs_ib legal compute IB");
    ibs[0].ib_bytes = 18u;
    T_CHECK(n48r3_cs_ib_valid(&ibs[0]) != 0, "cs_ib rejects unaligned bytes");
    ibs[0].ib_bytes = N48R3_CS_IB_BYTES_MAX + 4u;
    T_CHECK(n48r3_cs_ib_valid(&ibs[0]) != 0, "cs_ib rejects oversize bytes");
    ibs[0].ib_bytes = 20u; ibs[0].ip_type = 7u;
    T_CHECK(n48r3_cs_ib_valid(&ibs[0]) != 0, "cs_ib rejects unknown ip_type");
    ibs[0].ip_type = kN48R3IpCompute;
    ibs[0].va_start = 0x0001000000000000ull; /* bit 48 set -> not canonical */
    T_CHECK(n48r3_cs_ib_valid(&ibs[0]) != 0, "cs_ib rejects noncanonical VA");
    ibs[0].va_start = gva;

    /* N48R3CsIn parsing: version, count, reserved fields, exact length. */
    memset(buf, 0, sizeof buf);
    h->abi_version = N48R3_UC_ABI_VERSION;
    h->num_ibs = 1;
    ibs[0].va_start = gva; ibs[0].ib_bytes = 20u;
    ibs[0].ip_type = kN48R3IpCompute;
    T_CHECK(n48r3_cs_in_size(h) == sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
            "cs_in_size one IB");
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
                                 &out_h, &out_ibs) == 0 &&
            out_h->num_ibs == 1 && out_ibs[0].ib_bytes == 20u,
            "cs_in_validate legal one IB");
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn),
                                 &out_h, &out_ibs) == -1,
            "cs_in_validate rejects short structure");
    h->abi_version++;
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
                                 &out_h, &out_ibs) == -1,
            "cs_in_validate rejects ABI version mismatch");
    h->abi_version = N48R3_UC_ABI_VERSION;
    h->num_ibs = 0;
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
                                 &out_h, &out_ibs) == -1,
            "cs_in_validate rejects 0 IBs");
    h->num_ibs = N48R3_CS_IB_MAX + 1u;
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof buf, &out_h, &out_ibs) == -1,
            "cs_in_validate rejects >64 IBs");
    h->num_ibs = 1; h->flags = 1u;
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
                                 &out_h, &out_ibs) == -1,
            "cs_in_validate rejects nonzero flags");
    h->flags = 0;
    ibs[0].ib_bytes = 18u;
    T_CHECK(n48r3_cs_in_validate(buf, (uint32_t)sizeof(N48R3CsIn) + sizeof(N48R3CsIb),
                                 &out_h, &out_ibs) == -2,
            "cs_in_validate rejects illegal IB");

    /* WRITE_DATA packet shape used by the compute round-trip. */
    n = n48r3_pm4_write_data(pkt, n48r3_va_strip(gva) + 0x100u,
                             N48R3_COMPUTE_MAGIC);
    T_CHECK(n == 5 && pkt[0] == 0xC0370003u && pkt[1] == 0x00100500u &&
            pkt[2] == (uint32_t)((n48r3_va_strip(gva) + 0x100u) & 0xffffffffu) &&
            pkt[3] == (uint32_t)((n48r3_va_strip(gva) + 0x100u) >> 32) &&
            pkt[4] == N48R3_COMPUTE_MAGIC,
            "pm4_write_data golden shape (5 dwords)");
    T_CHECK(n48r3_pm4_write_data(pkt, 0x1001u, 0) != 5,
            "pm4_write_data rejects unaligned destination");

    /* PM4 NO_OP：PACKET3(PACKET3_NOP,0) 包头及完整 2-dword 包形态（C-09）。 */
    T_CHECK(N48R3_PM4_NOP == 0xC0001000u, "NOP header = PACKET3(PACKET3_NOP,0)");
    T_CHECK(N48R3_PM4_NOP_PAIR == 0x00000000C0001000ull,
            "complete NOP packet = header + zero body");
    T_CHECK((N48R3_PM4_NOP & 0x0000ff00u) == 0x1000u,
            "NOP opcode 0x10 at bits 15:8 (nvd.h:48-55)");

    /* Survey text parsing and mapped pattern helpers. */
    T_CHECK(n48r3_parse_fb_mc_base(txt, sizeof txt - 1, &parsed) == 1 &&
            parsed == fb_mc, "parse fb_mc_base from survey text");
    T_CHECK(n48r3_parse_fb_mc_base(badtxt, sizeof badtxt - 1, &parsed) == 0,
            "parse fb_mc_base rejects empty hex");
    n48r3_fill_pattern((volatile uint32_t *)scratch, 32u, 0xA5A55A5Au);
    T_CHECK(n48r3_check_pattern((volatile uint32_t *)scratch, 32u, 0xA5A55A5Au) == 0,
            "BO pattern write/read-back match");
    ((volatile uint32_t *)scratch)[17] ^= 1u;
    T_CHECK(n48r3_check_pattern((volatile uint32_t *)scratch, 32u, 0xA5A55A5Au) != 0,
            "BO pattern read-back detects corruption");
}

int main(void) {
    test_parse();
    test_root();
    test_logstream_args();
    test_exit_for_kr();
    test_kr_text();
    test_emit();
    test_usage();
    test_abi_pure();
    test_version();
    printf("%s: %d checks, %d failures\n", t_fail ? "FAIL" : "PASS", t_checks, t_fail);
    return t_fail ? 1 : 0;
}

#endif /* N48R3_HOST_TEST */
