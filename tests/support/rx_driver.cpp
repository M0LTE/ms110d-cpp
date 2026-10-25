// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rx_driver.hpp"

#include <algorithm>
#include <stdexcept>

namespace ms110d_test {

using namespace m0lte::ms110d;

RxDriver::RxDriver(InterleaverKind max_interleaver, bool trace_frames, bool turbo)
    : detector_(std::make_unique<PreambleDetector>()),
      receiver_(std::make_unique<DfeReceiver>()),
      trace_frames_(trace_frames) {
    DfeReceiverConfig config;
    config.max_interleaver = max_interleaver;
    config.turbo = turbo;
    const std::size_t bits = DfeReceiver::max_input_bits_for(max_interleaver);
    k7_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k7_133_171, bits);
    k9_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k9_561_753, bits);
    siso7_ = std::make_unique<m0lte::fec::TailBitingSisoDecoder>(m0lte::fec::k7_133_171, bits);
    siso9_ = std::make_unique<m0lte::fec::TailBitingSisoDecoder>(m0lte::fec::k9_561_753, bits);
    chain_ = std::make_unique<BasicChainBcjr<float>>(DfeReceiver::max_frame_symbols, 16);
    storage_.resize(DfeReceiver::storage_bytes(config));
    DfeDecoders d;
    d.viterbi_k7 = k7_.get();
    d.viterbi_k9 = k9_.get();
    d.siso_k7 = siso7_.get();
    d.siso_k9 = siso9_.get();
    d.chain = chain_.get();
    ring_.resize(std::max(DfeReceiver::ring_size_for(max_interleaver), PreambleDetector::ring_size));
    if (!stream_.configure(config.roll_off, ring_)) throw std::runtime_error("stream configure failed");
    if (!detector_->configure(PreambleDetectorConfig{}, stream_)) throw std::runtime_error("detector configure failed");
    if (!receiver_->configure(config, storage_, d, stream_)) throw std::runtime_error("configure failed");
}

RxRun RxDriver::run(std::span<const float> audio, std::size_t chunk) {
    RxRun out;
    current_ = &out;
    detector_->reset();
    receiver_->reset();
    if (trace_frames_) {
        receiver_->set_frame_trace(
            [](void* ctx, const DfeFrameTrace& t) noexcept {
                // A test hook: allocation here is the test's, outside any allocation scope.
                static_cast<RxDriver*>(ctx)->current_->frames.push_back(t);
            },
            this);
    } else {
        receiver_->set_frame_trace(nullptr, nullptr);
    }

    block_frames_mark_ = 0;
    stream_.reset();
    // One front end for both: per T/2 sample the detector first (its energies and, while searching,
    // the search), then the receiver, as the C# PushT2 does. `chunk` only sets how often the
    // events are collected, which cannot change a result.
    (void)chunk;
    for (std::size_t i = 0; i < audio.size() && !out.ended; ++i) {
        if (!stream_.push(audio[i])) continue;
        detector_->on_t2();
        if (detector_->has_detection()) {
            if (out.detected) break;  // only the first burst is recorded
            detector_->take_detection(out.detection);
            out.detected = true;
            if (!receiver_->start(dfe_start_from_detection(out.detection))) break;
            continue;  // the receiver starts on the next sample, as the C#'s tracking does
        }
        receiver_->on_t2();
        while (receiver_->event() != DfeEvent::None) {
            if (receiver_->event() == DfeEvent::BlockDecoded) {
                const DfeBlock& b = receiver_->block();
                RxBlockRecord rec;
                rec.index = b.index;
                rec.at_t2 = b.at_t2;
                rec.info.assign(b.info.begin(), b.info.end());
                rec.llrs.assign(b.llrs.begin(), b.llrs.end());
                rec.turbo_llrs.assign(b.turbo_llrs.begin(), b.turbo_llrs.end());
                rec.turbo = b.turbo;
                rec.mfb_offered = b.mfb_offered;
                rec.mfb_selected = b.mfb_selected;
                rec.relock_offers = b.relock_offers;
                rec.relock_adopted = b.relock_adopted;
                rec.frames_before = block_frames_mark_;
                rec.frames_end = out.frames.size();
                block_frames_mark_ = out.frames.size();
                out.blocks.push_back(std::move(rec));
            } else {
                out.end = receiver_->burst_end();
                out.ended = true;
            }
            receiver_->take_event();
        }
    }
    current_ = nullptr;
    return out;
}

}  // namespace ms110d_test
