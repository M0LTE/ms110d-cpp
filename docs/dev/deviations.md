# Accepted deviations

The C# package is the reference and the standard is the yardstick. When this library differs from the C# on purpose (for example a microcontroller build skipping a decoder pass and costing some dB), that is an accepted deviation. The rule from [the plan](../plan.md): no deviation exists only in prose. Each one is a named test with an issue number and a reason, and the test fails if the cost grows beyond what it states.

The published list is [docs/deviations.md](../deviations.md). It is generated; do not edit it by hand.

## Adding a deviation

1. Open a GitHub issue in this repo saying what differs, why we accept it, and what it costs. Its number is the deviation's identity.
2. Write the test in `tests/`, using `tests/support/deviation.hpp`. One macro declares the deviation, registers it for the list, and defines the Catch2 test (tagged `[deviation]` and `[issue-N]`, named from the number). The body returns the measured cost:

```cpp
MS110D_DEVIATION(123, "Skip the turbo pass", "Saves 40% CPU on the Cortex-M7",
                 "frames lost versus the C# on WN4 at 3 dB", "%", 0.5) {
    return measure_frames_lost_percent();
}
```

3. Regenerate the list and commit it with the test:

        cmake --build build --target update-deviations

`ms110d_tests --list-tests "[deviation]"` lists every deviation test. Because the declaration, the bound and the test are one construct, none can exist without the others and the bound in the list is the bound that is checked. The issue number must be a plain integer literal.

## Writing the measurement

- State every deviation as a cost where lower is better, for example "frames lost versus the C#, %" or "SNR needed above the C#, dB". The bound is the most the cost may be.
- Measurements must be deterministic: fixed seeds, and counts rather than wall-clock time. A CPU cost is a count (operations, decoder iterations, samples processed), never a timing, so the test gives the same answer on every machine.
- The test fails if the cost is above the bound. It also fails if the cost is below half the bound, because then the bound is stale and no longer guards anything: tighten it to just above the measured cost. This is safe because the measurement is deterministic.
- If a change makes the cost larger, either fix the regression, or raise the bound and record why in the issue; the regenerated list then shows the change in review.
- A deviation that applies only to some builds (for example single precision only) is still always declared, so the list is identical in every build. Inside the body, call `SKIP("reason")` where it does not apply:

```cpp
MS110D_DEVIATION(124, "Single precision equaliser", "Float is enough on the Cortex-M4",
                 "frames lost versus the C#, double reference", "%", 1.0) {
#ifndef MS110D_SINGLE_PRECISION
    SKIP("only applies to the single precision build");
#endif
    return measure_frames_lost_percent();
}
```

- Every deviation must actually run, not skip, in at least one CI build; a deviation that skips everywhere is never measured and so exists only in prose. Reviewers check this on every PR that adds or changes a deviation: say in the PR which CI build measures it.
- Because of the half-bound rule, a deviation can never be zero-cost. If the cost falls to nothing, the difference is gone: delete the deviation and close its issue.

## The list check

The ctest `deviations_doc` regenerates the list from the declarations and compares it with `docs/deviations.md`, and fails if they differ (for example a deviation added or its bound changed without regenerating). `cmake/DeviationsDoc.cmake` does the work, in the same style as the [API lock](api-lock.md). The generator is the hidden Catch2 test tagged `[.deviation-doc]`, which writes to the file named by `MS110D_DEVIATIONS_OUT`.

The helper's own behaviour (including SKIP for build-dependent deviations) is tested in `tests/deviation_test.cpp` with deviations built directly rather than through the macro, so they never reach the list.

## Later

The side-by-side tests against the C# package (coming in a later PR) will follow the same convention: a deviation found there gets an issue, a declared and tested bound, and an entry in the same list.
