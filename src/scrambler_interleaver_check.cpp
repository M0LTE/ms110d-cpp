// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Compiles the header-only scramblers and the interleaver as part of the library (so the
// Cortex-M7 build checks them), and instantiates the interleaver for the element types the
// modem uses: hard bits and soft values.

#include "interleaver.hpp"
#include "scrambler.hpp"

namespace m0lte::ms110d {

template void Interleaver::interleave<std::uint8_t>(const std::uint8_t*, std::uint8_t*) const noexcept;
template void Interleaver::deinterleave<std::uint8_t>(const std::uint8_t*, std::uint8_t*) const noexcept;
template void Interleaver::interleave<float>(const float*, float*) const noexcept;
template void Interleaver::deinterleave<float>(const float*, float*) const noexcept;
template void Interleaver::interleave<double>(const double*, double*) const noexcept;
template void Interleaver::deinterleave<double>(const double*, double*) const noexcept;

static_assert(Scrambler().state() == 1u);
static_assert(Interleaver(192, 25).load_location(8) == 8u);
static_assert(Interleaver(0, 5).load_location(3) == 0u);

}  // namespace m0lte::ms110d
