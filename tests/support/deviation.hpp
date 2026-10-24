// Accepted deviations: see docs/dev/deviations.md.
//
// A deviation is a difference from the C# reference that we keep on purpose.
// Declare it once at namespace scope, then measure it in a Catch2 test:
//
//   inline const auto kTurboSkip = ms110d_test::declare_deviation(
//       {123, "Skip the turbo pass", "Saves 40% CPU on the Cortex-M7",
//        "SNR cost on WN4 at 1% frame error", "dB", 0.5});
//
//   TEST_CASE("Deviation #123: skip the turbo pass", "[deviation][issue-123]") {
//       kTurboSkip.check(measure_snr_cost());
//   }
//
// Declaring also registers the deviation, so the generated list in
// docs/deviations.md includes it. The test fails if the measured value is above
// the stated bound.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ms110d_test {

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
    // can be tested directly.
    Result evaluate(double measured) const {
        char buf[128];
        const bool ok = std::isfinite(measured) && measured <= bound;
        std::snprintf(buf, sizeof buf, "measured %g %s, bound %g %s", measured, unit.c_str(), bound, unit.c_str());
        std::string msg = ok ? std::string("Deviation #") + std::to_string(issue) + " within bound: " + buf
                             : std::string("Deviation #") + std::to_string(issue) + " (" + title + ") exceeds its stated bound: " + buf +
                                   " (" + metric + "). Fix the regression, or raise the bound in the test and in issue #" +
                                   std::to_string(issue) + " with a reason.";
        return {ok, msg};
    }

    // Call from the TEST_CASE. Fails the test, naming the issue, if the
    // measured value is above the bound.
    void check(double measured) const {
        const Result r = evaluate(measured);
        INFO(r.message);
        CHECK(r.ok);
    }
};

inline std::vector<Deviation>& deviation_registry() {
    static std::vector<Deviation> registry;
    return registry;
}

inline void require_valid(const Deviation& d) {
    const bool bad = d.issue <= 0 || d.title.empty() || d.reason.empty() || d.metric.empty() || d.unit.empty() ||
                     !std::isfinite(d.bound);
    if (bad) {
        std::fprintf(stderr, "Invalid deviation declaration (issue %d): issue, title, reason, metric, unit and a finite bound are all required\n",
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

// Validates the deviation and adds it to the published list.
inline Deviation declare_deviation(Deviation d) {
    require_valid(d);
    deviation_registry().push_back(d);
    return d;
}

// The published list as markdown, in issue order. This is what
// docs/deviations.md must contain.
inline std::string deviations_markdown() {
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
        char bound[64];
        std::snprintf(bound, sizeof bound, "%g", d.bound);
        out += "## #" + std::to_string(d.issue) + ": " + d.title + "\n\n";
        out += "- Issue: https://github.com/M0LTE/ms110d-cpp/issues/" + std::to_string(d.issue) + "\n";
        out += "- Reason: " + d.reason + "\n";
        out += "- Measured: " + d.metric + "\n";
        out += std::string("- Bound: at most ") + bound + " " + d.unit + "\n\n";
    }
    return out;
}

}  // namespace ms110d_test
