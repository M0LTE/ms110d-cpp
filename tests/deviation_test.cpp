#include "deviation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

using ms110d_test::Deviation;

// These are not real deviations: they are built directly, not declared, so they
// never reach the published list.
TEST_CASE("deviation helper passes within the bound and fails above it", "[deviation-selftest]") {
    const Deviation d{9999, "Self test", "Not a real deviation", "made-up cost", "dB", 0.5};

    CHECK(d.evaluate(0.0).ok);
    CHECK(d.evaluate(0.5).ok);
    CHECK(d.evaluate(-3.0).ok);

    const auto over = d.evaluate(0.51);
    CHECK_FALSE(over.ok);
    CHECK(over.message.find("#9999") != std::string::npos);
    CHECK(over.message.find("exceeds") != std::string::npos);

    CHECK_FALSE(d.evaluate(std::nan("")).ok);
}

TEST_CASE("deviation markdown says so when there are none", "[deviation-selftest]") {
    // Only meaningful while the registry is empty; once real deviations exist
    // the generated list is checked against docs/deviations.md instead.
    if (!ms110d_test::deviation_registry().empty()) SKIP("real deviations are registered");
    CHECK(ms110d_test::deviations_markdown().find("no accepted deviations yet") != std::string::npos);
}

// Writes the generated list to the file named by MS110D_DEVIATIONS_OUT. Run by
// cmake/DeviationsDoc.cmake; not part of the normal ctest run.
TEST_CASE("write deviations markdown", "[deviation-doc]") {
    const char* path = std::getenv("MS110D_DEVIATIONS_OUT");
    REQUIRE(path != nullptr);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << ms110d_test::deviations_markdown();
}
