// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace ms110d_tool {

// Resamples to 9600 Hz the way scripts/corpus/curate.py does: zero-stuff by up, low-pass with an
// 801-tap Kaiser (beta 8) windowed-sinc FIR with a 4400 Hz cutoff at rate_in * up, keep every
// down-th sample, evaluated as a polyphase sum in double and aligned to the group delay, then
// rounded to 16 bits. For 12000 Hz that is up 4, down 5, which is what the corpus uses. The input
// rate must be at least 9600 Hz; 9600 itself is only quantised.
// Returns false (and leaves `out` empty) if the rate is unusable.
bool resample_to_9600(const std::vector<double>& in, int rate_in, std::vector<std::int16_t>& out);

}  // namespace ms110d_tool
