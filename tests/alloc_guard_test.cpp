#include "alloc_guard.hpp"

#include <m0lte/ms110d/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_CASE("the allocation counter sees heap use", "[alloc]") {
    const ms110d_test::AllocationScope scope;
    int* p = new int(1);
    const std::size_t seen = scope.allocations();
    delete p;
    CHECK(seen == 1);
}

TEST_CASE("version() does not allocate", "[alloc][version]") {
    const ms110d_test::AllocationScope scope;
    const auto v = m0lte::ms110d::version();
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(v.major == 0);
}
