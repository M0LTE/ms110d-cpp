// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <m0lte/ms110d/mode.hpp>

namespace m0lte::ms110d {

// Transmit configuration for the 3 kHz waveform: the C# Ms110dTxSettings, with the same
// defaults. Internal for now (not part of the locked public API). The amplitude is a float
// and the roll-off a double, as in the C#, whatever the precision option.
struct TxSettings {
    // Waveform number, 0 to 8 or 13. Default WN 6 (3200 bps QPSK).
    int waveform_number = 6;
    // Interleaver length option (WID d5d4). Default Short.
    InterleaverKind interleaver = InterleaverKind::Short;
    // Convolutional constraint length, 7 or 9 (WID d3). Default 7.
    int constraint_length = 7;
    // Preamble super-frame repeats M, 1 to 32 (D.5.2.1.3). Default 3.
    int preamble_superframes = 3;
    // TLC section blocks N, 0 to 255 (D.5.2.1.2; 0 omits the section). Default 0.
    int tlc_blocks = 0;
    // Append the 32-bit EOM (D.5.4.3) after the data. Default on.
    bool append_eom = true;
    // Extend the final mini-probe by 32 symbols as an EOT marker (D.5.4.4). Default on.
    bool append_eot = true;
    // Linear scale applied to the unit-magnitude symbol stream before pulse shaping.
    float amplitude = 0.5f;
    // SRRC pulse-shaping roll-off, 0.05 to 1. Default 0.35, the D.5.1.5 recommendation.
    double roll_off = 0.35;
};

}  // namespace m0lte::ms110d
