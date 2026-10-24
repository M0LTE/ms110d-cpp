// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Test support: the preamble detector and the equaliser receive path run on one stream the way
// the C# Ms110dDemodulator runs them (the detector's event hands the burst to the receiver at the
// very sample the C# locks and re-fits), recording the first burst's frames, blocks and end. The
// public receive API that does this for real is a later story; this is only for the tests.

#include "bcjr.hpp"
#include "dfe_receiver.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "preamble_detector.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ms110d_test {

struct RxBlockRecord {
    int index = 0;
    std::int64_t at_t2 = 0;
    std::vector<std::uint8_t> info;
    std::vector<float> llrs;        // first pass
    std::vector<float> turbo_llrs;  // the last turbo iterate, empty if none
    m0lte::ms110d::DfeTurbo turbo = m0lte::ms110d::DfeTurbo::Skipped;
    bool mfb_offered = false;
    bool mfb_selected = false;
    int relock_offers = 0;
    int relock_adopted = 0;
    std::size_t frames_before = 0;  // frames traced before this block's first frame
    std::size_t frames_end = 0;     // ... and after its last
};

struct RxRun {
    bool detected = false;
    m0lte::ms110d::PreambleDetection detection{};
    std::vector<m0lte::ms110d::DfeFrameTrace> frames;
    std::vector<RxBlockRecord> blocks;
    bool ended = false;
    m0lte::ms110d::DfeBurstEnd end{};
};

class RxDriver {
public:
    explicit RxDriver(m0lte::ms110d::InterleaverKind max_interleaver = m0lte::ms110d::InterleaverKind::Short,
                      bool trace_frames = true, bool turbo = true);

    // A fresh stream: everything reset, the first burst decoded, fed in chunks of `chunk`.
    RxRun run(std::span<const float> audio, std::size_t chunk = 2400);

    m0lte::ms110d::DfeReceiver& receiver() { return *receiver_; }

private:
    std::unique_ptr<m0lte::ms110d::PreambleDetector> detector_;
    std::unique_ptr<m0lte::ms110d::DfeReceiver> receiver_;
    std::unique_ptr<m0lte::fec::TailBitingViterbiDecoder> k7_;
    std::unique_ptr<m0lte::fec::TailBitingViterbiDecoder> k9_;
    std::unique_ptr<m0lte::fec::TailBitingSisoDecoder> siso7_;
    std::unique_ptr<m0lte::fec::TailBitingSisoDecoder> siso9_;
    std::unique_ptr<m0lte::ms110d::BasicChainBcjr<float>> chain_;
    std::size_t block_frames_mark_ = 0;
    std::vector<std::uint8_t> storage_;
    bool trace_frames_;
    RxRun* current_ = nullptr;
};

}  // namespace ms110d_test
