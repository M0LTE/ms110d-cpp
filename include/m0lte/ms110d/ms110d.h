#ifndef M0LTE_MS110D_MS110D_H
#define M0LTE_MS110D_MS110D_H

// C interface to the MS110D modem, for callers that are not C++ (.NET, Python,
// C firmware). The conventions every entry point follows are in
// docs/dev/c-interface.md. In short:
//   - every function is named ms110d_*, returns a status, and never throws;
//   - only fixed-width integers, pointers and plain structs cross the boundary;
//   - objects are opaque handles made and freed by the library, with all memory
//     sized when the handle is created and none allocated afterwards;
//   - the caller owns every input and output buffer and passes its length;
//   - ms110d_abi_version() changes whenever anything here changes shape.
// Public headers use // comments only (see docs/dev/api-lock.md).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Symbol export. The shared library is built with hidden visibility, so only
// functions marked MS110D_API are visible to a loader. Define
// MS110D_C_STATIC when linking a static build of the C interface on Windows.
#if defined(_WIN32) && !defined(MS110D_C_STATIC)
#if defined(MS110D_C_BUILDING)
#define MS110D_API __declspec(dllexport)
#else
#define MS110D_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define MS110D_API __attribute__((visibility("default")))
#else
#define MS110D_API
#endif

// The shape of the C interface this header describes. Bumped on any change that
// could break a caller that was built against an older header.
#define MS110D_ABI_VERSION 1u

// Only fixed-width integers cross the boundary: no enum, bool or size_t types
// in signatures or structs, because their size is not the same on every ABI
// (arm-none-eabi makes enums one byte). The names below are plain constants.
//
// A status is an int32_t: zero is success, anything else is an error.
typedef int32_t ms110d_status;
enum {
    MS110D_OK = 0,
    MS110D_ERR_INVALID_ARGUMENT = 1,
    MS110D_ERR_BUFFER_TOO_SMALL = 2,
    MS110D_ERR_UNSUPPORTED = 3,
    MS110D_ERR_STATE = 4,
    MS110D_ERR_INTERNAL = 5,
    // The library could not allocate (create only; nothing allocates afterwards).
    MS110D_ERR_OUT_OF_MEMORY = 6,
    // Caller-supplied memory for an init function is too small or misaligned.
    MS110D_ERR_BAD_MEMORY = 7
};

// The floating point type the library was built with, as the width in bits.
enum {
    MS110D_PRECISION_SINGLE = 32,
    MS110D_PRECISION_DOUBLE = 64
};

// The ABI version the loaded library implements. Compare it with
// MS110D_ABI_VERSION before calling anything else. Always safe to call.
MS110D_API uint32_t ms110d_abi_version(void);

// The library's own version (the CMake project version). Each output pointer
// must be non-null, else MS110D_ERR_INVALID_ARGUMENT and nothing is written.
MS110D_API ms110d_status ms110d_version(int32_t* major, int32_t* minor, int32_t* patch);

// The precision the library was built with, so a caller can check it matches
// the one it expects. Fails with MS110D_ERR_INVALID_ARGUMENT on a null pointer.
MS110D_API ms110d_status ms110d_precision_bits(int32_t* precision);

// A fixed, static, human-readable name for a status. Never null, never freed by
// the caller. A value from a newer library, or a bad one, gives "unknown status".
MS110D_API const char* ms110d_status_string(ms110d_status status);

#ifdef __cplusplus
}
#endif

#endif
