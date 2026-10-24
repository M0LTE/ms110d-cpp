// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Small helpers shared by the equaliser receive path's source files (dfe_receiver.cpp,
// dfe_turbo.cpp). Internal; inline arithmetic only, compiled under this repo's flags.

#include "dsp/complex_f.hpp"
#include "equaliser.hpp"
#include "tables.hpp"

#include <array>
#include <cstdint>

namespace m0lte::ms110d::dfe_detail {

using dsp::ComplexF;

inline ComplexF cf(EqSample v) noexcept { return {v.real(), v.imag()}; }
inline EqSample eq(ComplexF v) noexcept { return {v.re, v.im}; }
inline ComplexF psk8(int symbol) noexcept { return cf(tables::psk8[static_cast<std::size_t>(symbol)]); }

// Math.Clamp: min if below, max if above (the arguments here always have min <= max).
inline double clamp(double v, double lo, double hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// Ring symbol -> tribit, the inverse of the 8PSK transcoding (the C# SymbolToTribit8).
inline constexpr std::array<std::uint8_t, 8> symbol_to_tribit8 = [] {
    std::array<std::uint8_t, 8> inv{};
    for (std::uint8_t t = 0; t < 8; ++t) inv[tables::transcode_8psk[t]] = t;
    return inv;
}();

}  // namespace m0lte::ms110d::dfe_detail
