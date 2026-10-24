#pragma once

namespace m0lte::ms110d {

// The floating point type used by the modem. Chosen at build time with the
// CMake option MS110D_PRECISION (double by default, or single).
#ifdef MS110D_SINGLE_PRECISION
using real = float;
#else
using real = double;
#endif

}  // namespace m0lte::ms110d
