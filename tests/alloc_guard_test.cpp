// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "alloc_guard.hpp"

#include <m0lte/ms110d/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <new>

TEST_CASE("the allocation counter sees heap use", "[alloc]") {
    const ms110d_test::AllocationScope scope;
    // Direct calls to the allocation functions cannot be optimised away,
    // unlike a new/delete pair on a pointer that never escapes.
    void* p = ::operator new(sizeof(int));
    const std::size_t seen = scope.allocations();
    ::operator delete(p);
    CHECK(seen == 1);
}

TEST_CASE("version() does not allocate", "[alloc][version]") {
    const ms110d_test::AllocationScope scope;
    (void)m0lte::ms110d::version();
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
}
