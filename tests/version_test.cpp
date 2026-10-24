#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

TEST_CASE("version() matches the CMake project version", "[version]") {
    const auto v = m0lte::ms110d::version();
    const std::string text = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    CHECK(text == MS110D_EXPECTED_VERSION);
}

TEST_CASE("real follows the precision option", "[config]") {
#ifdef MS110D_SINGLE_PRECISION
    STATIC_REQUIRE(std::is_same_v<m0lte::ms110d::real, float>);
#else
    STATIC_REQUIRE(std::is_same_v<m0lte::ms110d::real, double>);
#endif
}
