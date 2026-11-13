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

#define N48R3_SERVICE_NAME "NaviR3Bringup"
#define N48R3_SEL_INFO     0u
#define N48R3_SEL_TRIGGER  1u
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

int main(void) {
    test_parse();
    test_root();
    test_logstream_args();
    test_exit_for_kr();
    test_kr_text();
    test_emit();
    test_usage();
    test_version();
    printf("%s: %d checks, %d failures\n", t_fail ? "FAIL" : "PASS", t_checks, t_fail);
    return t_fail ? 1 : 0;
}

#endif /* N48R3_HOST_TEST */
