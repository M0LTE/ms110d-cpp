# .NET interop tests

`tests/interop` (`Ms110dInterop.Tests`, xunit v3, net10.0) loads the C# reference from NuGet and the C++ shared library through the C interface in one process. It is the harness later stages extend to compare the two stage by stage. It has smoke tests and the transmit cross-checks (below). The smoke tests: the ABI version and library version match, the precision is reported, and a C# `Ms110dModulator` loads and modulates a few bits.

## Running

Through ctest, which builds the shared library and tells the tests where it is:

    cmake -S . -B build
    cmake --build build -j4
    ctest --test-dir build --output-on-failure

The test `dotnet_interop` runs `dotnet run` on the project, with `MS110D_NATIVE_LIB` set to the built `ms110d_c` and `MS110D_EXPECTED_VERSION` to the CMake project version. It needs a .NET 10 SDK and network access to restore NuGet packages.

`MS110D_DOTNET_TESTS` is `AUTO` by default: the test is registered when a .NET 10 SDK is on the PATH and silently left out when not. `ON` makes a missing SDK an error, `OFF` leaves the test out (CI uses `OFF` for the C++ matrix and the sanitizer job, which do not need it; a sanitizer-built library cannot be loaded into an uninstrumented .NET process anyway).

The tests are an xunit v3 executable, run with `dotnet run` rather than `dotnet test`: on the .NET 10 SDK `dotnet test` drives xunit v3 through Microsoft.Testing.Platform, which needs extra SDK configuration and silently ignores a VSTest-style `--filter`. `dotnet run` just runs the exe, which exits non-zero on failure, and takes xunit's own switches after `--`.

To run it by hand, set `MS110D_NATIVE_LIB`, `MS110D_EXPECTED_VERSION` and `MS110D_PIN_FILE` (ctest sets all three; the pin file is `reference/pin.txt`) and run `dotnet run --project tests/interop -c Release`. One class: `dotnet run --project tests/interop -c Release -- -class Ms110dInterop.Tests.SmokeTests`; one method: `-- -method Ms110dInterop.Tests.SmokeTests.PrecisionIsReported`. The build directory can be anywhere; nothing is located relative to the source tree.

## The reference version

The NuGet package version is taken from `reference/pin.txt` (`nuget_version`) by `tests/interop/Directory.Build.props`, as an exact version range. It is the same pin that selects the reference files; see docs/dev/reference-files.md for the rule for changing it.

## Adding a comparison

Declare the new entry point in `Native.cs`, call it and the C# equivalent on the same input in a test, and compare. The library path is resolved in one place (`Native.LibraryPath`), so tests never hard-code it.

## Transmit cross-checks (`TxCrossCheckTests`)

Three tests per combination, for every waveform number (0 to 8 and 13), interleaver length and constraint length the standard defines (78 combinations):

- **Side by side** (layer 2, issue 21). A seeded random payload and random settings (preamble super-frames, TLC blocks, EOM, EOT, amplitude, roll-off) go through the C# `Ms110dModulator` and the C++ transmitter (P/Invoke, `ms110d_tx_modulate` on even rounds and push/read in random chunk sizes on odd ones). The lengths must be equal and every sample within 1e-6 (the tolerance in docs/dev/modulator.md). Payload lengths include 0, 1, and the block edges.
- **C++ transmit, C# receive on a clean channel** (issue 22). The C# `Ms110dDemodulator` must decode the C++ audio, reporting the EOM end reason, the waveform, interleaver and constraint length the preamble announced, and the payload bit for bit.
- **The same in AWGN** at 20 dB SNR in 3 kHz from `M0LTE.HfChannel` (a test-only package reference), well above every AWGN mask of Table D-LXIV, so a failure is a transmit fault and not the receiver's limit.

Seeds are plain arithmetic on (test, waveform, interleaver, code, round), so a run is repeatable, and every failure message carries the seed, payload length and settings to replay it.

Payload lengths are chosen around the block size B (the info bits per interleaver block): B - 32 (the EOM ends exactly at a block end), B (the EOM then needs a second block), B + 1, and 2B + 5. The side-by-side test runs all four for every combination, so every combination covers several blocks. The receive tests run, by default, B - 32 and B + 1 on the clean channel and 2B + 5 in AWGN. A planted fault that shifts the payload by a bit in every block after the first fails all 78 combinations of the side-by-side, B + 1 and AWGN tests.

The receive tests are spread over several classes only so that xunit runs them in parallel. The default run is about 5 CPU minutes and under 2 minutes on a 16 core machine. `MS110D_LONG_TESTS=1` in the environment runs 25 rounds of random settings per combination and random lengths up to four blocks (no cap) side by side, and 12 rounds per receive test. A failure is a bug in one implementation, to be settled against the standard; never loosen a check to pass.
