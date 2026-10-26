// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Which mini-probes of a burst are cyclically shifted (MIL-STD-188-110D D.5.2.2). The one rule,
// for the transmitter and the receiver alike; issue 68 is the decision behind it.
//
// Data frames are numbered from 0 across the whole burst. The probe after frame f is shifted only
// when f mod L == L - 2, L the number of frames in the waveform's LONG interleaver (256 for WN 1
// and 2, 128 for WN 3 and 4, 64 for WN 5 to 13), whatever interleaver is selected. The probe that
// ends the preamble (f = -1) is never shifted, and the EOT is a cyclic extension of the last probe
// as sent. The C# up to 0.2.1 instead shifted at the selected interleaver's boundary,
// (f + 2) % frames == 0, which differs from this for the ultra-short, short and medium
// interleavers; the Long interleaver and WN 0 (no probes) are the same.

#include <cstdint>

namespace m0lte::ms110d {

constexpr bool probe_is_shifted(std::int64_t frame_number, int long_frames) noexcept {
    return frame_number >= 0 && frame_number % long_frames == long_frames - 2;
}

}  // namespace m0lte::ms110d
