# C interface

A small C interface sits on top of the C++ library so that .NET, Python, C firmware and anything else with a C FFI can use it. The header is `include/m0lte/ms110d/ms110d.h` (C99, `extern "C"` guarded), the implementation is `src/c_api.cpp`, and the shared library is the CMake target `ms110d_c` (`libms110d_c.so`, `ms110d_c.dll`). The C++ static library `m0lte::ms110d` is unchanged and is still what C++ consumers and microcontrollers link.

Every later entry point follows the conventions below. If a new function cannot, change the conventions here first.

## Conventions

- **Names.** Every function, type and macro starts with `ms110d_` or `MS110D_`. Nothing from C++ crosses the boundary: no classes, templates, references, `bool`, `size_t` in structs or `std::` anything. Fixed-width integers (`int32_t`, `uint32_t`) in signatures and structs.
- **Status codes.** Every function that can fail returns `ms110d_status`; `MS110D_OK` is 0. Results come back through output pointers. No exception ever crosses the boundary (the core has none), and `ms110d_status_string()` turns a status into a fixed text; it takes an `int32_t`, not the enum, because an out-of-range value converted to an enum is undefined behaviour in C++ (the sanitizer job caught this). The same applies to any function that takes a value from the caller: take an integer and validate it. A null output pointer is `MS110D_ERR_INVALID_ARGUMENT` and nothing is written. The exceptions are functions that cannot fail, which return their value directly (`ms110d_abi_version()`).
- **Opaque handles.** Objects are forward-declared structs used only through pointers (`typedef struct ms110d_thing ms110d_thing;`). `ms110d_thing_create(config, &handle)` makes one and `ms110d_thing_destroy(handle)` frees it; the library owns the memory, the caller never sees inside. Destroy accepts null. A handle is used by one thread at a time; separate handles are independent.
- **No allocation after creation.** `create` sizes everything the object will ever need from its configuration (mode, interleaver, code) and allocates it then. After `create` returns, no entry point allocates. The allocation guard test (docs/dev/allocation-guard.md) holds the C++ side to this.
- **Caller-owned buffers.** Input and output buffers belong to the caller and are passed as a pointer plus an explicit length in elements. A function never keeps a pointer past its return. Where the output size depends on the input, there is a sizing function (for example `ms110d_thing_audio_length(handle, bits, &n)`) so the caller can allocate exactly. An output buffer too small gives `MS110D_ERR_BUFFER_TOO_SMALL`, with the needed length written to the `*_out_len` parameter where one exists; nothing partial is promised.
- **Sample type.** Audio crosses the boundary as `float` (32 bit), as the C# does, whatever the library's internal precision.
- **Precision query.** The library is built for `float` or `double` inside. `ms110d_precision_bits()` returns `MS110D_PRECISION_SINGLE` (32) or `MS110D_PRECISION_DOUBLE` (64), so a caller that depends on one can check it before it trusts any numbers.
- **ABI version.** `MS110D_ABI_VERSION` in the header and `ms110d_abi_version()` in the library. A caller compares them before anything else and refuses a mismatch. Bump it on any change that can break an already-built caller (changed signature, struct layout or enum value). Adding a function does not need a bump while the library is 0.x, but a new header with new functions on an old library is caught by the symbol failing to resolve. The ABI version is independent of the library's semantic version.
- **Structs.** Plain structs passed by pointer, with an explicit `uint32_t struct_size` first member filled in by the caller when a struct may grow, so old callers keep working.
- **Enums.** C enums with explicit values; never reordered or renumbered.

## Export

The shared library is built with hidden visibility. Only functions marked `MS110D_API` are exported, and the ctest `c_exports` checks (on Linux) that nothing else is. On Windows `MS110D_API` is `dllexport` while building and `dllimport` for users.

`MS110D_BUILD_C_SHARED` controls the target. It is ON for a top-level host build and OFF for cross builds, so the Cortex-M7 build stays a static C++ library. It is installed with the rest, as `m0lte::ms110d_c`, when on.

## Tests

`tests/c_api_test.c` is compiled as C99 (not C++), with warnings as errors, and calls every function including the null and unknown-value cases. Add each new function to it. The header is also under the API lock (docs/dev/api-lock.md), so any change to it fails `api_lock` until the snapshot is deliberately updated. The .NET project in `tests/interop` calls the same functions through P/Invoke (docs/dev/dotnet-interop.md).
