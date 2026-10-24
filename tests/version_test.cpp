#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_CASE("version() reports 0.0.0", "[version]") {
    const auto v = m0lte::ms110d::version();
    CHECK(v.major == 0);
    CHECK(v.minor == 0);
    CHECK(v.patch == 0);
}

TEST_CASE("real follows the precision option", "[config]") {
#ifdef MS110D_SINGLE_PRECISION
    STATIC_REQUIRE(std::is_same_v<m0lte::ms110d::real, float>);
#else
    STATIC_REQUIRE(std::is_same_v<m0lte::ms110d::real, double>);
#endif
}
