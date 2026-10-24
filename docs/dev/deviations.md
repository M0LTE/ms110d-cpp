# Accepted deviations

The C# package is the reference and the standard is the yardstick. When this library differs from the C# on purpose (for example a microcontroller build skipping a decoder pass and costing some dB), that is an accepted deviation. The rule from [the plan](../plan.md): no deviation exists only in prose. Each one is a named test with an issue number and a reason, and the test fails if the cost grows beyond what it states.

The published list is [docs/deviations.md](../deviations.md). It is generated; do not edit it by hand.

## Adding a deviation

1. Open a GitHub issue in this repo saying what differs, why we accept it, and what it costs. Its number is the deviation's identity.
2. Write the test in `tests/`, using `tests/support/deviation.hpp`. Declare the deviation at namespace scope, then measure it in a test tagged `[deviation]` and `[issue-N]`:

```cpp
inline const auto kTurboSkip = ms110d_test::declare_deviation(
    {123, "Skip the turbo pass", "Saves 40% CPU on the Cortex-M7",
     "SNR cost on WN4 at 1% frame error", "dB", 0.5});

TEST_CASE("Deviation #123: skip the turbo pass", "[deviation][issue-123]") {
    kTurboSkip.check(measure_snr_cost());
}
```

3. Regenerate the list and commit it with the test:

        cmake --build build --target update-deviations

`--list-tests "[deviation]"` on the test binary lists every deviation test.

The bound is the most the measured value may be. Make it the measured cost plus a small margin, not a round guess. If a later change makes the cost larger, the test fails with a message naming the issue. Either fix the regression, or raise the bound in the test and record why in the issue; the regenerated list then shows the change in review.

## The list check

The ctest `deviations_doc` regenerates the list from the declarations and compares it with `docs/deviations.md`, and fails if they differ (for example a deviation added or its bound changed without regenerating). `cmake/DeviationsDoc.cmake` does the work, in the same style as the [API lock](api-lock.md). The generator is the hidden Catch2 test tagged `[deviation-doc]`, which writes to the file named by `MS110D_DEVIATIONS_OUT`.

Declaring and testing are separate steps, so keep the declaration next to its test. A declaration without a test would show in the list with nothing enforcing it; review for that.

The helper's own behaviour is tested in `tests/deviation_test.cpp` with deviations that are not declared, so they never reach the list.

## Later

The side-by-side tests against the C# package (coming in a later PR) will follow the same convention: a deviation found there gets an issue, a declared and tested bound, and an entry in the same list.
