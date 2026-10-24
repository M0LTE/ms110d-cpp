# .NET interop tests

`tests/interop` (`Ms110dInterop.Tests`, xunit v3, net10.0) loads the C# reference from NuGet and the C++ shared library through the C interface in one process. It is the harness later stages extend to compare the two stage by stage. Today it has smoke tests: the ABI version and library version match, the precision is reported, and a C# `Ms110dModulator` loads and modulates a few bits.

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
