// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>

namespace ms110d_test {

// Total calls to global operator new since the program started. The counting
// operator new/delete live in alloc_guard.cpp and apply to the whole test binary.
std::size_t allocation_count() noexcept;

// Counts heap allocations made between construction and allocations().
// Read the count first and only then assert on it, because the test framework
// itself allocates when it reports.
class AllocationScope {
public:
    AllocationScope() noexcept : start_(allocation_count()) {}
    std::size_t allocations() const noexcept { return allocation_count() - start_; }

private:
    std::size_t start_;
};

}  // namespace ms110d_test
