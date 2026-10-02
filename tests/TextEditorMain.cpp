/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "tests/Test.h"
#include "tests/TestHarness.h"
#include "tools/flags/CommandLineFlags.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

static DEFINE_string2(match, m, nullptr, "Substring match to filter which tests to run.");
static DEFINE_bool2(quiet, q, false, "Only print failing tests.");
static DEFINE_bool2(verbose, v, false, "Enable verbose test output.");

TestHarness CurrentTestHarness() {
    return TestHarness::kDM;
}

class SimpleReporter : public skiatest::Reporter {
public:
    SimpleReporter() = default;

    void resetForTest() {
        fFailed = false;
        fFailures.clear();
    }

    void reportFailed(const skiatest::Failure& failure) override {
        fFailed = true;
        fTotalFailures++;
        fFailures.push_back(failure.toString());
    }

    bool failed() const { return fFailed; }
    const std::vector<SkString>& failures() const { return fFailures; }
    int totalFailures() const { return fTotalFailures; }

    bool verbose() const override { return FLAGS_verbose; }

private:
    bool fFailed = false;
    int fTotalFailures = 0;
    std::vector<SkString> fFailures;
};

int main(int argc, char** argv) {
    CommandLineFlags::SetUsage("Standalone test runner for text editor unit tests.");
    CommandLineFlags::Parse(argc, argv);

    std::vector<const skiatest::Test*> tests;
    for (const skiatest::Test& test : skiatest::TestRegistry::Range()) {
        tests.push_back(&test);
    }

    std::sort(tests.begin(), tests.end(), [](const skiatest::Test* a, const skiatest::Test* b) {
        return strcmp(a->fName, b->fName) < 0;
    });

    SimpleReporter reporter;
    int passedCount = 0;
    int failedCount = 0;
    int skippedCount = 0;

    for (const skiatest::Test* test : tests) {
        if (CommandLineFlags::ShouldSkip(FLAGS_match, test->fName)) {
            skippedCount++;
            continue;
        }

        if (test->fTestType != skiatest::TestType::kCPU &&
            test->fTestType != skiatest::TestType::kCPUSerial) {
            skippedCount++;
            continue;
        }

        reporter.resetForTest();
        skiatest::ReporterContext ctx(&reporter, SkString(test->fName));

        skiatest::Timer timer;
        test->cpu(&reporter);
        double elapsedMs = timer.elapsedMs();

        if (reporter.failed()) {
            failedCount++;
            SkDebugf("[FAIL] %s (%.2f ms)\n", test->fName, elapsedMs);
            for (const auto& failMsg : reporter.failures()) {
                SkDebugf("       %s\n", failMsg.c_str());
            }
        } else {
            passedCount++;
            if (!FLAGS_quiet) {
                SkDebugf("[OK]   %s (%.2f ms)\n", test->fName, elapsedMs);
            }
        }
    }

    SkDebugf("=================================================================\n");
    SkDebugf("Ran %d tests (%d passed, %d failed, %d skipped).\n",
             passedCount + failedCount, passedCount, failedCount, skippedCount);

    return failedCount > 0 ? 1 : 0;
}
