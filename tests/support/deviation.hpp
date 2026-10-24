// Accepted deviations: see docs/dev/deviations.md.
//
// A deviation is a difference from the C# reference that we keep on purpose.
// One construct declares it, registers it and defines its test, so none of the
// three can exist without the others:
//
//   MS110D_DEVIATION(123, "Skip the turbo pass", "Saves 40% CPU on the Cortex-M7",
//                    "frames lost versus the C# on WN4 at 3 dB", "%", 0.5) {
//       return measure_frames_lost_percent();
//   }
//
// The issue number must be a plain integer literal. The body returns the
// measured cost (lower is better) and is the body of a Catch2 test tagged
// [deviation] and [issue-123]. The test fails if the cost is above the bound,
// and also if it is below half the bound (the bound is stale: tighten it).
// A deviation that applies to only some builds is always declared; the body
// calls SKIP() where it does not apply.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ms110d_test {
namespace deviation_detail {

// Shortest text that reads back as the same double, so the published list and
// the check cannot disagree about the bound.
inline std::string number(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

struct Deviation {
    int issue;               // GitHub issue number in M0LTE/ms110d-cpp
    std::string title;       // short name, as in the issue title
    std::string reason;      // why we keep the difference
    std::string metric;      // what is measured, for example "SNR cost on WN4"
    std::string unit;        // unit of the metric, for example "dB"
    double bound;            // the most the metric may be; larger fails

    struct Result {
        bool ok;
        std::string message;
    };

    // Compares a measured value with the bound. Does not touch Catch2, so it
    // can be tested directly. Lower is better. Measurements must be
    // deterministic (fixed seeds, counts rather than wall-clock time), so a
    // value below half the bound means the bound is stale.
    Result evaluate(double measured) const {
        const bool finite = std::isfinite(measured);
        const bool over = !finite || measured > bound;
        const bool stale = finite && measured < bound / 2;
        const std::string id = "Deviation #" + std::to_string(issue) + " (" + title + ")";
        const std::string numbers = "measured " + number(measured) + " " + unit + ", bound " + number(bound) + " " + unit + " (" + metric + ")";
        if (over)
            return {false, id + " exceeds its stated bound: " + numbers + ". Fix the regression, or raise the bound and record why in issue #" +
                               std::to_string(issue) + "."};
        if (stale)
            return {false, id + " is below half its stated bound: " + numbers + ". Tighten the bound to just above the measured cost."};
        return {true, id + " within bound: " + numbers};
    }
};

inline std::vector<Deviation>& deviation_registry() {
    static std::vector<Deviation> registry;
    return registry;
}

inline void require_valid(const Deviation& d) {
    const bool bad = d.issue <= 0 || !(d.bound > 0) || d.title.empty() || d.reason.empty() || d.metric.empty() || d.unit.empty() ||
                     !std::isfinite(d.bound);
    if (bad) {
        std::fprintf(stderr, "Invalid deviation declaration (issue %d): issue, title, reason, metric, unit and a finite positive bound are all required\n",
                     d.issue);
        std::abort();
    }
    for (const Deviation& other : deviation_registry()) {
        if (other.issue == d.issue) {
            std::fprintf(stderr, "Deviation #%d is declared twice\n", d.issue);
            std::abort();
        }
    }
}

// Validates a deviation and adds it to the published list. Only the
// MS110D_DEVIATION macro should create one of these.
struct Registered {
    Deviation d;
    explicit Registered(Deviation dev) : d(std::move(dev)) {
        require_valid(d);
        deviation_registry().push_back(d);
    }
};

// The check run by the test the macro defines.
inline void check(const Deviation& d, double measured) {
    const Deviation::Result r = d.evaluate(measured);
    INFO(r.message);
    CHECK(r.ok);
}

}  // namespace deviation_detail

// The published list as markdown, in issue order. This is what
// docs/deviations.md must contain.
inline std::string deviations_markdown() {
    using namespace deviation_detail;
    std::vector<Deviation> all = deviation_registry();
    std::sort(all.begin(), all.end(), [](const Deviation& a, const Deviation& b) { return a.issue < b.issue; });

    std::string out =
        "<!-- Generated from the deviation tests. Do not edit; run: cmake --build <build-dir> --target update-deviations -->\n"
        "# Accepted deviations\n\n"
        "Differences from the C# reference that we keep on purpose. Each one is a test that measures it and fails if it grows past the bound stated here. See [docs/dev/deviations.md](dev/deviations.md).\n\n";
    if (all.empty()) {
        out += "There are no accepted deviations yet.\n";
        return out;
    }
    for (const Deviation& d : all) {
        out += "## #" + std::to_string(d.issue) + ": " + d.title + "\n\n";
        out += "- Issue: https://github.com/M0LTE/ms110d-cpp/issues/" + std::to_string(d.issue) + "\n";
        out += "- Reason: " + d.reason + "\n";
        out += "- Measured: " + d.metric + "\n";
        out += std::string("- Bound: at most ") + number(d.bound) + " " + d.unit + "\n\n";
    }
    return out;
}

}  // namespace ms110d_test

#define MS110D_DEVIATION(issue, title, reason, metric, unit, bound)                                     \
    static double ms110d_dev_measure_##issue();                                                         \
    static const ::ms110d_test::deviation_detail::Registered ms110d_dev_reg_##issue{                    \
        ::ms110d_test::deviation_detail::Deviation{issue, title, reason, metric, unit, bound}};         \
    TEST_CASE("Deviation #" #issue ": " title, "[deviation][issue-" #issue "]") {                       \
        ::ms110d_test::deviation_detail::check(ms110d_dev_reg_##issue.d, ms110d_dev_measure_##issue()); \
    }                                                                                                   \
    static double ms110d_dev_measure_##issue()
