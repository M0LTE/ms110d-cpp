// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "deviation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <limits>

using ms110d_test::deviation_detail::Deviation;

// These are not real deviations: they are built directly rather than through
// MS110D_DEVIATION, so they never reach the published list.
TEST_CASE("deviation helper passes within the bound and fails outside it", "[deviation-selftest]") {
    const Deviation d{9999, "Self test", "Not a real deviation", "made-up cost", "dB", 0.5};

    CHECK(d.evaluate(0.5).ok);
    CHECK(d.evaluate(0.25).ok);
    CHECK(d.evaluate(0.4).ok);

    const auto over = d.evaluate(0.51);
    CHECK_FALSE(over.ok);
    CHECK(over.message.find("#9999") != std::string::npos);
    CHECK(over.message.find("exceeds") != std::string::npos);

    // A cost below half the bound means the bound is stale.
    const auto stale = d.evaluate(0.2);
    CHECK_FALSE(stale.ok);
    CHECK(stale.message.find("Tighten") != std::string::npos);
    CHECK_FALSE(d.evaluate(0.0).ok);
    CHECK_FALSE(d.evaluate(-3.0).ok);

    CHECK_FALSE(d.evaluate(std::numeric_limits<double>::quiet_NaN()).ok);
    CHECK_FALSE(d.evaluate(std::numeric_limits<double>::infinity()).ok);
}

TEST_CASE("bounds print with round-trip precision", "[deviation-selftest]") {
    const double awkward = 0.1 + 0.2;
    CHECK(std::strtod(ms110d_test::deviation_detail::number(awkward).c_str(), nullptr) == awkward);
    CHECK(ms110d_test::deviation_detail::number(0.5) == "0.5");
}

TEST_CASE("deviation markdown says so when there are none", "[deviation-selftest]") {
    // Only meaningful while the registry is empty; once real deviations exist
    // the generated list is checked against docs/deviations.md instead.
    if (!ms110d_test::deviation_detail::deviation_registry().empty()) SKIP("real deviations are registered");
    CHECK(ms110d_test::deviations_markdown().find("no accepted deviations yet") != std::string::npos);
}

// The pattern for a deviation that applies to some builds only: it is always
// declared (so the list is the same in every build) and the body skips where it
// does not apply. This shows SKIP ends the measurement without running the
// check, using a lambda in place of a macro body. The skipped result is
// expected and is not a failure.
TEST_CASE("a build-dependent deviation skips instead of being absent", "[deviation-selftest]") {
    const Deviation d{9998, "Self test", "Not a real deviation", "made-up cost", "dB", 0.5};
    auto measure = []() -> double {
#ifdef MS110D_SINGLE_PRECISION
        return 0.4;
#else
        SKIP("only applies to the single precision build");
        return 0.0;  // not reached; 0.0 would fail as a stale bound
#endif
    };
    ms110d_test::deviation_detail::check(d, measure());
}

// Writes the generated list to the file named by MS110D_DEVIATIONS_OUT. Run by
// cmake/DeviationsDoc.cmake; hidden, so it is not part of a normal run.
TEST_CASE("write deviations markdown", "[.deviation-doc]") {
    const char* path = std::getenv("MS110D_DEVIATIONS_OUT");
    REQUIRE(path != nullptr);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << ms110d_test::deviations_markdown();
}
