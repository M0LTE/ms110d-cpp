// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Single precision complex arithmetic written out the way the C# Complex32 does it (a plain
// struct of two floats, a product formed as (ac - bd, ad + bc), the magnitude a float square
// root), so a receiver stage ported from the C# gives the same floats to the last bit. It is not
// std::complex<float>, whose product may take a slow path for infinities and NaNs and whose
// layout and rounding we do not control. Header only and inline, no allocation, no exceptions.
//
// This is internal (src/dsp is not installed) and holds only inline arithmetic that is safe under
// any floating point flags: the contraction of a multiply and an add into a fused operation
// would change the results, so the translation units that include it must be built with
// -ffp-contract=off, as every target of this repo is (docs/dev/build-notes.md).

#include <cmath>

namespace m0lte::dsp {

struct ComplexF {
    float re = 0.0f;
    float im = 0.0f;
};

inline ComplexF operator+(ComplexF a, ComplexF b) noexcept { return {a.re + b.re, a.im + b.im}; }
inline ComplexF operator-(ComplexF a, ComplexF b) noexcept { return {a.re - b.re, a.im - b.im}; }
inline ComplexF operator*(ComplexF a, ComplexF b) noexcept {
    return {(a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re)};
}
inline ComplexF operator*(ComplexF a, float s) noexcept { return {a.re * s, a.im * s}; }
inline ComplexF conj(ComplexF a) noexcept { return {a.re, -a.im}; }
inline float mag2(ComplexF a) noexcept { return (a.re * a.re) + (a.im * a.im); }
inline float abs(ComplexF a) noexcept { return std::sqrt((a.re * a.re) + (a.im * a.im)); }
inline float arg(ComplexF a) noexcept { return std::atan2(a.im, a.re); }
// cos(phase) + j sin(phase) in single precision; pass a negated phase for the conjugate rotation.
inline ComplexF from_phase(float phase) noexcept { return {std::cos(phase), std::sin(phase)}; }

}  // namespace m0lte::dsp
