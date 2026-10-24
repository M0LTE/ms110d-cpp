# Allocation guard

The core must not allocate on the heap after start-up. The test binary replaces the global `operator new` and `operator delete` (`tests/support/alloc_guard.cpp`) with versions that count calls.

In a test, make an `ms110d_test::AllocationScope` before the code under test, read `scope.allocations()` straight after it, and only then assert on the number. Read it first because the test framework allocates when it reports.

    const ms110d_test::AllocationScope scope;
    run_the_thing();
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);

`tests/alloc_guard_test.cpp` shows it on `version()`, and checks that the counter does see an allocation. Set up (construct the modem, size its buffers) before the scope starts; everything inside it must be allocation free. The hook only sees `operator new`, not direct `malloc`, which the core does not use.
