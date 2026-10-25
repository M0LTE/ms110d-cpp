// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Test support for the performance mask tests (docs/dev/masks.md): one burst of audio through the
// preamble detector and then the Walsh (WN 0) or equaliser (WN 1 and up) receiver, whichever the
// preamble names, returning every decoded information bit up to and including the block that holds
// the EOM, as the C# Ms110dDemodulator's BlockDecoded events do. The public receive API is a
// separate story; this is only for the tests.

#include "bcjr.hpp"
#include "dfe_receiver.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "preamble_detector.hpp"
#include "wn0_receiver.hpp"

#include <cstdint>
#include <memory>
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
    void run_wn0(std::span<const float> audio, std::size_t used, MaskBurst& out);
    void run_dfe(std::span<const float> audio, std::size_t used, MaskBurst& out);

    std::unique_ptr<m0lte::ms110d::PreambleDetector> detector_;
    std::unique_ptr<m0lte::ms110d::DfeReceiver> dfe_;
    std::unique_ptr<m0lte::ms110d::Wn0Receiver> wn0_;
    std::unique_ptr<m0lte::fec::TailBitingViterbiDecoder> k7_, k9_, wn0_k7_, wn0_k9_;
    std::unique_ptr<m0lte::fec::TailBitingSisoDecoder> siso7_, siso9_;
    std::unique_ptr<m0lte::ms110d::BasicChainBcjr<float>> chain_;
    std::vector<std::uint8_t> storage_;
    std::vector<m0lte::ms110d::real> wn0_block_;
    m0lte::ms110d::PreambleDetection detection_{};
};

}  // namespace ms110d_test
