// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "filter_design.hpp"

#include <cmath>
#include <numbers>

namespace m0lte::dsp {

// Same expression order as the C# FilterDesign.RootRaisedCosine, so the two agree to the
// last bit of the double wherever both use the same libm.
double root_raised_cosine(double t, double beta) noexcept {
    constexpr double pi = std::numbers::pi;

    if (std::fabs(t) < 1e-8) {
        return 1 - beta + (4 * beta / pi);
    }

    if (beta > 0 && std::fabs(std::fabs(t) - (1 / (4 * beta))) < 1e-8) {
        return beta / std::sqrt(2.0) *
               (((1 + (2 / pi)) * std::sin(pi / (4 * beta))) + ((1 - (2 / pi)) * std::cos(pi / (4 * beta))));
    }

    const double numerator = std::sin(pi * t * (1 - beta)) + (4 * beta * t * std::cos(pi * t * (1 + beta)));
    const double denominator = pi * t * (1 - (16 * beta * beta * t * t));
    return numerator / denominator;
}

}  // namespace m0lte::dsp
