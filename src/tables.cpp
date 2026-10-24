// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Compiles the internal tables into the library so every target, including the Cortex-M7
// cross build, proves they are valid constexpr data. Nothing is emitted here.

#include "tables.hpp"

namespace m0lte::ms110d::tables {

static_assert(fixed_pn.size() == 256 && cnt_pn.size() == 256 && wid_pn.size() == 256);
static_assert(find_probe_spec(48)->base.size() == 25);
static_assert(probe_symbol(*find_probe_spec(24), 0, true) == base13[6]);
static_assert(probe_symbol(*find_probe_spec(24), 7, false) == base13[7]);

}  // namespace m0lte::ms110d::tables
