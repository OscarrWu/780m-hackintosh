// test_admission.cpp - host tests for the v1.7 run-level prerequisite model.
//
// Pure header-only checks (no IOKit): the depth floor/plan, the admission evaluation, and
// the exact strings the driver publishes as the stop line, admission line and status field
// (requirement C0.9/E5/V15; tech plan v2.4 §5.2/§8.5).
#include "rdna3_survey.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static bool contains(const char *hay, const char *needle) {
    return hay && needle && std::strstr(hay, needle) != nullptr;
}

static R3AdmissionInputs good_inputs(void) {
    R3AdmissionInputs in {};
    in.survey_aborted = false;
    in.disc_present = true;
    in.mp0_1 = true;
    in.mp1_1 = true;
    in.gc_0 = true;
    in.sdma_0 = true;
    in.mmhub_1 = true;
    in.dcn_2 = true;
    in.fb_dual_seen = true;
    in.fb_dual_ok = true;
    return in;
}

static void test_depth_plan(void) {
    CHECK(r3_stage_survey_floor(0u) == 0u);
    CHECK(r3_stage_survey_floor(1u) == 2u);
    CHECK(r3_stage_survey_floor(5u) == 2u);

    R3DepthPlan p = r3_stage_depth_plan(1u, kR3DepthAbsent, 0u);
    CHECK(p.floor == 2u);
    CHECK(p.effective == 2u);
    CHECK(!p.present);
    CHECK(std::strcmp(p.reason, "default-floor") == 0);

    p = r3_stage_depth_plan(1u, kR3DepthExplicit, 3u);
    CHECK(p.effective == 3u);
    CHECK(p.present);
    CHECK(std::strcmp(p.reason, "explicit") == 0);

    p = r3_stage_depth_plan(1u, kR3DepthExplicit, 1u);
    CHECK(p.requested == 1u);
    CHECK(p.effective == 2u);
    CHECK(std::strcmp(p.reason, "raised-to-floor") == 0);

    p = r3_stage_depth_plan(1u, kR3DepthInvalid, 0u);
    CHECK(p.effective == 2u);
    CHECK(std::strcmp(p.reason, "invalid-as-default") == 0);

    p = r3_stage_depth_plan(0u, kR3DepthAbsent, 0u);
    CHECK(p.floor == 0u);
    CHECK(p.effective == 0u);
}

static void test_admission(void) {
    R3AdmissionInputs in = good_inputs();
    R3AdmissionResult r = r3_admission_eval(in);
    CHECK(r.ok);
    CHECK(r.missing[0] == '\0');
    CHECK(std::strcmp(r.fb_dual, "ok") == 0);

    in.disc_present = false;
    r = r3_admission_eval(in);
    CHECK(!r.ok);
    CHECK(contains(r.missing, "disc"));

    in = good_inputs();
    in.gc_0 = false;
    in.dcn_2 = false;
    r = r3_admission_eval(in);
    CHECK(!r.ok);
    CHECK(contains(r.missing, "gc"));
    CHECK(contains(r.missing, "dcn"));

    in = good_inputs();
    in.survey_aborted = true;
    r = r3_admission_eval(in);
    CHECK(!r.ok);
    CHECK(r.missing[0] == '\0'); /* the abort line carries the reason */

    in = good_inputs();
    in.fb_dual_seen = false;
    r = r3_admission_eval(in);
    CHECK(r.ok);
    CHECK(std::strcmp(r.fb_dual, "missing") == 0);

    in = good_inputs();
    in.fb_dual_ok = false;
    r = r3_admission_eval(in);
    CHECK(r.ok); /* advisory: preparation is not gated on it */
    CHECK(std::strcmp(r.fb_dual, "fail") == 0);
}

static void test_lines(void) {
    char buf[256];
    const R3DepthPlan plan = r3_stage_depth_plan(1u, kR3DepthAbsent, 0u);

    R3AdmissionInputs in = good_inputs();
    in.gc_0 = false;
    const R3AdmissionResult bad = r3_admission_eval(in);
    r3_admission_fail_line(buf, sizeof buf, bad, plan);
    CHECK(contains(buf, "admission: FAIL missing="));
    CHECK(contains(buf, "requested=0 effective=2 floor=2"));

    r3_admission_pass_line(buf, sizeof buf, 2u, "ok");
    CHECK(std::strcmp(buf, "admission: PASS depth=2 fb-dual=ok") == 0);

    r3_admission_summary(buf, sizeof buf, 2u, "ok");
    CHECK(std::strcmp(buf, "admission=PASS depth=2 fb-dual=ok") == 0);

    r3_stop_line(buf, sizeof buf, "stage2 prep FAIL: prep:mmhub-unresolved");
    CHECK(std::strcmp(buf, "stop: stage2 prep FAIL: prep:mmhub-unresolved") == 0);
    r3_stop_line(buf, sizeof buf, nullptr);
    CHECK(std::strcmp(buf, "stop: (no reason emitted)") == 0);

    const R3DepthPlan raised = r3_stage_depth_plan(1u, kR3DepthExplicit, 1u);

    r3_status_line(buf, sizeof buf, true, "admission=PASS depth=2 fb-dual=ok", "ignored",
                   &raised);
    CHECK(std::strcmp(buf, "armed, not fired; admission=PASS depth=2 fb-dual=ok; depth "
                           "requested=1 effective=2 floor=2 reason=raised-to-floor\n") == 0);
    r3_status_line(buf, sizeof buf, true, "admission=PASS depth=2 fb-dual=ok", "ignored",
                   nullptr);
    CHECK(std::strcmp(buf, "armed, not fired; admission=PASS depth=2 fb-dual=ok\n") == 0);
    r3_status_line(buf, sizeof buf, false, nullptr, "admission: FAIL missing=disc", &plan);
    CHECK(std::strcmp(buf, "stopped: admission: FAIL missing=disc, not fired; depth "
                           "requested=0 effective=2 floor=2\n") == 0);
}

int main(void) {
    test_depth_plan();
    test_admission();
    test_lines();
    if (failures) {
        std::printf("test_admission: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_admission: OK\n");
    return 0;
}
