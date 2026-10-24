// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Filter design primitives. This module knows nothing of MS110D: it is laid out so it can
// move to its own library when a second modem needs it (same pattern as src/fec/). It is
// the C++ counterpart of the parts of M0LTE.Dsp that the modulator uses (FilterDesign
// .RootRaisedCosine); the rest of that package is not ported until something needs it.
//
// Design maths always runs in double and is independent of the modem's precision option, so
// the module has no precision-keyed types. It never allocates and never throws.
namespace m0lte::dsp {

// Root-raised-cosine pulse, evaluated in symbol units: t = 0 is the symbol centre and +-1
// the neighbouring symbols. beta is the roll-off and must be in [0, 1] (the C# throws
// outside that range; here the result is unspecified, so callers validate). The two
// removable singularities (t = 0 and |t| = 1 / (4 beta)) are evaluated as limits. An
// RRC-shaped signal occupies about symbol_rate * (1 + beta).
double root_raised_cosine(double t, double beta) noexcept;

}  // namespace m0lte::dsp
