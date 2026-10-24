#include <m0lte/ms110d/ms110d.h>

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstdint>

// The only translation unit that defines the C interface. Nothing here may
// throw or allocate; the core has no exceptions anyway.

extern "C" {

uint32_t ms110d_abi_version(void) {
    return MS110D_ABI_VERSION;
}

ms110d_status ms110d_version(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major == nullptr || minor == nullptr || patch == nullptr) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    const auto v = m0lte::ms110d::version();
    *major = v.major;
    *minor = v.minor;
    *patch = v.patch;
    return MS110D_OK;
}

ms110d_status ms110d_precision_bits(ms110d_precision* precision) {
    if (precision == nullptr) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    static_assert(sizeof(m0lte::ms110d::real) == 4 || sizeof(m0lte::ms110d::real) == 8, "real must be float or double");
    *precision = sizeof(m0lte::ms110d::real) == 4 ? MS110D_PRECISION_SINGLE : MS110D_PRECISION_DOUBLE;
    return MS110D_OK;
}

const char* ms110d_status_string(int32_t status) {
    switch (status) {
    case static_cast<int32_t>(MS110D_OK): return "ok";
    case static_cast<int32_t>(MS110D_ERR_INVALID_ARGUMENT): return "invalid argument";
    case static_cast<int32_t>(MS110D_ERR_BUFFER_TOO_SMALL): return "buffer too small";
    case static_cast<int32_t>(MS110D_ERR_UNSUPPORTED): return "unsupported";
    case static_cast<int32_t>(MS110D_ERR_STATE): return "invalid state";
    case static_cast<int32_t>(MS110D_ERR_INTERNAL): return "internal error";
    }
    return "unknown status";
}

}  // extern "C"
