// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Test support for the performance mask tests (docs/dev/masks.md): one burst of audio through the
// public Receiver, returning every decoded information bit up to and including the block that holds
// the EOM, as the C# Ms110dDemodulator's BlockDecoded events do.

#include <m0lte/ms110d/receiver.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace ms110d_test {

struct MaskBurst {
    bool detected = false;
    int waveform = -1;
    bool eom = false;                  // the burst ended on the EOM
    std::vector<std::uint8_t> bits;    // decoded information bits, one per byte
};

class MaskDriver {
public:
    // Sized for the Long interleaver, the one the masks are specified at.
    MaskDriver();
    MaskBurst run(std::span<const float> audio);

private:
    m0lte::ms110d::Receiver rx_;
    std::vector<std::uint8_t> memory_;
};

}  // namespace ms110d_test
