# C interface

A small C interface sits on top of the C++ library so that .NET, Python, C firmware and anything else with a C FFI can use it. The header is `include/m0lte/ms110d/ms110d.h` (C99, `extern "C"` guarded), the implementation is `src/c_api.cpp`, and the shared library is the CMake target `ms110d_c` (`libms110d_c.so`, `ms110d_c.dll`). The C++ static library `m0lte::ms110d` is unchanged and is still what C++ consumers and microcontrollers link.

Every later entry point follows the conventions below. If a new function cannot, change the conventions here first.

## Conventions

- **Names.** Every function, type and macro starts with `ms110d_` or `MS110D_`. Nothing from C++ crosses the boundary: no classes, templates, references, `bool`, `size_t` in structs or `std::` anything. Fixed-width integers (`int32_t`, `uint32_t`) in signatures and structs.
- **Fixed-width types only.** No enum, `bool` or `size_t` ever appears in a function signature or a boundary struct; only `int32_t`, `uint32_t`, `int64_t`, `uint8_t`, `float`, `double`, pointers and plain structs of those. Their size is the same on every ABI, whereas an enum is one byte on arm-none-eabi and four elsewhere. Names such as `MS110D_OK` and `MS110D_PRECISION_DOUBLE` are anonymous-enum constants only. `tests/c_layout_check.c` holds `_Static_assert`s on the size of every boundary type, compiled for the host by ctest and for the Cortex-M7 in CI; add each new struct there.
- **Status codes.** Every function that can fail returns `ms110d_status`, which is a `typedef int32_t`; `MS110D_OK` is 0. Results come back through output pointers. No exception ever crosses the boundary (the core has none), and `ms110d_status_string()` turns a status into a fixed text; a status is a plain integer precisely so that an unknown or bad value is answered with "unknown status" and is never an out-of-range enum conversion (undefined behaviour in C++, which the sanitizer job caught). Any value the caller supplies is an integer that the library validates. Codes include `MS110D_ERR_OUT_OF_MEMORY` (create could not allocate) and `MS110D_ERR_BAD_MEMORY` (caller-supplied memory too small or misaligned). A null output pointer is `MS110D_ERR_INVALID_ARGUMENT` and nothing is written. The exceptions are functions that cannot fail, which return their value directly (`ms110d_abi_version()`).
- **Thread safety.** The plain functions (`ms110d_abi_version`, `ms110d_version`, `ms110d_precision_bits`, `ms110d_status_string`) are thread-safe: no state, no allocation. A handle is not thread-safe; use it from one thread at a time (moving it between threads with your own synchronisation is fine). Separate handles are independent and may run on separate threads.
- **Opaque handles.** Objects are forward-declared structs used only through pointers (`typedef struct ms110d_thing ms110d_thing;`). `ms110d_thing_create(config, &handle)` makes one and `ms110d_thing_destroy(handle)` frees it; the library owns the memory, the caller never sees inside. Destroy accepts null. A handle is used by one thread at a time; separate handles are independent.
- **Two ways to get memory.** Every handle type offers both: `ms110d_thing_create(config, &handle)` / `ms110d_thing_destroy(handle)` where the library allocates once, and, for firmware with no heap, `ms110d_thing_memory_size(config, &bytes, &alignment)` plus `ms110d_thing_init(void* memory, uint32_t bytes, config, &handle)`, which builds the object in caller-supplied memory (`MS110D_ERR_BAD_MEMORY` if it is too small or misaligned) and needs no destroy. `create` is a thin wrapper over the same code, so the two cannot drift. Sizes use `uint32_t`, not `size_t`.
- **No allocation after creation.** `create` sizes everything the object will ever need from its configuration (mode, interleaver, code) and allocates it then. After `create` returns, no entry point allocates. The allocation guard test (docs/dev/allocation-guard.md) holds the C++ side to this.
- **Caller-owned buffers.** Input and output buffers belong to the caller and are passed as a pointer plus an explicit length in elements. A function never keeps a pointer past its return. Where the output size depends on the input, there is a sizing function (for example `ms110d_thing_audio_length(handle, bits, &n)`) so the caller can allocate exactly. An output buffer too small gives `MS110D_ERR_BUFFER_TOO_SMALL`, with the needed length written to the `*_out_len` parameter where one exists; nothing partial is promised.
- **Sample type.** Audio crosses the boundary as `float` (32 bit), as the C# does, whatever the library's internal precision.
- **Precision query.** The library is built for `float` or `double` inside. `ms110d_precision_bits()` returns `MS110D_PRECISION_SINGLE` (32) or `MS110D_PRECISION_DOUBLE` (64), so a caller that depends on one can check it before it trusts any numbers.
- **ABI version.** `MS110D_ABI_VERSION` in the header and `ms110d_abi_version()` in the library. A caller compares them before anything else and refuses a mismatch. Bump it on any change that can break an already-built caller (changed signature, struct layout or enum value). Adding a function does not need a bump while the library is 0.x, but a new header with new functions on an old library is caught by the symbol failing to resolve. The ABI version is independent of the library's semantic version.
- **Structs.** Plain structs passed by pointer, with an explicit `uint32_t struct_size` first member filled in by the caller when a struct may grow, so old callers keep working. Policy for an input struct: a `struct_size` smaller than the first published size is `MS110D_ERR_INVALID_ARGUMENT`; one larger than the library knows is accepted only if every byte beyond the known size is zero (otherwise `MS110D_ERR_INVALID_ARGUMENT`, so a field the library does not know is never silently ignored); a smaller-than-known size from an older caller, once a struct has grown, takes the defaults for the missing fields. Reserved words must be zero. For output structs the library fills only `struct_size` bytes.
- **Named constants.** Status codes, kinds and similar values are named constants with explicit values, never reordered or renumbered. The types that carry them across the boundary are fixed-width integers, never C enum types, because enum size varies between compilers (1 byte on bare-metal ARM by default).

## Events: poll and drain

Results the modem produces (a decoded block, a finished burst, soft decisions) are delivered by polling, never by callback. After the caller pushes samples with `ms110d_thing_process(handle, samples, count)`, it calls `ms110d_thing_next_event(handle, &event, ...)` repeatedly until that reports there is nothing left (status `MS110D_OK` with `event.kind == MS110D_EVENT_NONE`). Why not callbacks: a managed callback needs a pinned delegate with a lifetime the library cannot see, allows the library to be re-entered from inside itself, makes exceptions cross the boundary, and does not suit a microcontroller main loop. Polling has none of those problems, and the order of events is simply the queue order.

- **Queue.** The handle owns a fixed-capacity event queue sized at creation. If a caller does not drain and it fills, `process` returns `MS110D_ERR_BUFFER_TOO_SMALL` before consuming input that would overflow it, so nothing is dropped silently.
- **Event struct.** Plain, with `uint32_t struct_size` first. The caller sets it to the size of the struct it was compiled with; the library fills only that many bytes and never reads or writes beyond it, so a struct can grow in a later version without an ABI bump. `kind` is a `uint32_t` constant, not an enum. Unknown kinds are skipped by the caller.
- **Payload arrays are copied into caller buffers.** The alternative, a pointer into the handle valid until the next call, is cheaper but is a trap for garbage-collected callers (the array can be read after it was overwritten) and for any caller that queues events. So `next_event` takes the destination buffer and its capacity as parameters; the event reports the length needed. If the buffer is too small the call returns `MS110D_ERR_BUFFER_TOO_SMALL`, fills the length, and leaves the event queued so the caller can retry with a bigger buffer.
- **Sketch**, based on the C# `Ms110dDemodulator` events `BlockDecoded` (block index and bits), `BurstCompleted` (payload bits, end reason, block count, lock info) and the per-block LLR diagnostics:

      typedef struct ms110d_event {
          uint32_t struct_size;   /* set by the caller */
          uint32_t kind;          /* MS110D_EVENT_NONE, _BLOCK_DECODED, _BURST_COMPLETED, _BLOCK_LLRS */
          int32_t  block_index;   /* BLOCK_DECODED, BLOCK_LLRS */
          uint32_t payload_len;   /* elements needed in the buffer */
          int32_t  end_reason;    /* BURST_COMPLETED: which D.5.4.5 exit */
          int32_t  blocks;        /* BURST_COMPLETED: input-data blocks decoded */
          /* lock info fields follow, only ever appended */
      } ms110d_event;

      ms110d_status ms110d_demod_next_event(ms110d_demod* h, ms110d_event* ev,
                                            void* payload, uint32_t payload_capacity);

  `payload` holds bits (one per byte) for the decoded and completed events and `float` LLRs for the LLR events; which one is given by `kind`. This is a sketch of the convention, not a commitment to those names; the stage that adds the demodulator settles the details and updates this section.

## Open points for the demodulator

To settle in [#27](https://github.com/M0LTE/ms110d-cpp/issues/27), before the first receive entry point lands, so the signatures don't change later:

- `process` reports how many samples it consumed, so a caller can resume after the event queue fills part-way.
- Every event carries the sample position it relates to, so tests can match events with the C# one for one.
- A finished burst in C# carries the whole burst's bits. Either the maximum burst length is fixed at creation so the queue can hold it, or bits are delivered one block at a time and the burst event only closes the burst.
- Soft-decision events are switched on at creation, so a microcontroller build doesn't pay for them.

## Export

The shared library is built with hidden visibility. Only functions marked `MS110D_API` are exported, and the ctest `c_exports` checks (on Linux) that the exported functions are exactly the `MS110D_API` ones the header declares. Where the linker accepts it (checked with `check_linker_flag`) a version script (`cmake/ms110d_c.map`) enforces it: the standard library's templates are visible by default and would otherwise leak into the export table as weak symbols in an unoptimised build. On Windows `MS110D_API` is `dllexport` while building and `dllimport` for users.

`MS110D_BUILD_C_SHARED` controls the target. It is ON for a top-level host build and OFF for cross builds, so the Cortex-M7 build stays a static C++ library. It is installed with the rest, as `m0lte::ms110d_c`, when on.

## Transmit

`ms110d_tx_*` (handle `ms110d_tx`, settings `ms110d_tx_settings`) is the first handle type and the model for the rest: create/destroy, `ms110d_tx_memory_size` plus `ms110d_tx_init` for caller memory, `ms110d_tx_audio_length` for sizing, `ms110d_tx_push_bits` and `ms110d_tx_read` with caller buffers, plus `ms110d_tx_modulate` for a whole burst in one call (`MS110D_ERR_BUFFER_TOO_SMALL` with the length needed in `*written`), no events. `read` with capacity 0 is an error, so a count of 0 written only ever means the burst is finished. The settings struct has `struct_size` first, flags as `int32_t` 0 or 1 and an explicit `reserved` word so the `double` sits at offset 40 on every ABI; `ms110d_tx_settings_default` fills it from the C++ defaults so the two cannot drift. How it works is in docs/dev/transmitter.md. Adding these functions did not need an ABI bump (the library is 0.x), so `MS110D_ABI_VERSION` stays 1.

## Tests

`tests/c_api_test.c` is compiled as C99 (not C++), with warnings as errors, and calls every function including the null and unknown-value cases. Add each new function to it. The header is also under the API lock (docs/dev/api-lock.md), so any change to it fails `api_lock` until the snapshot is deliberately updated. `tests/c_transmitter_test.cpp` (the `ms110d_c_tests` executable) drives the transmit functions through the shared library and compares with the C# reference audio. The .NET project in `tests/interop` calls the same functions through P/Invoke (docs/dev/dotnet-interop.md).
