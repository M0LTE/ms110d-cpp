// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace m0lte::ms110d {

// The floating point type used by the modem. Chosen at build time with the
// CMake option MS110D_PRECISION (double by default, or single). The inline
// namespace is keyed on the precision, so a program built with one precision
// fails to link against a library built with the other instead of mismatching.
#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
using real = float;
}
#else
inline namespace v_double {
using real = double;
}
#endif

}  // namespace m0lte::ms110d
