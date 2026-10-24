// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <m0lte/ms110d/version.hpp>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif
Version version() noexcept {
    return library_version;
}
}

}  // namespace m0lte::ms110d
