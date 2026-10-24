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
    if (!receiver_->configure(config, storage_, d)) throw std::runtime_error("configure failed");
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
    std::size_t det = 0;  // samples the detector has used
    std::size_t rx = 0;   // samples the receiver has used
    while (rx < audio.size() && !out.ended) {
        if (!receiver_->active() && receiver_->event() == DfeEvent::None) {
            if (out.detected) break;  // the burst never ended: out of audio
            // Searching: the detector leads, the receiver's front end follows to the same sample.
            const std::size_t n = std::min(chunk, audio.size() - det);
            det += detector_->process(audio.subspan(det, n));
            while (rx < det) rx += receiver_->push_audio(audio.subspan(rx, det - rx));
            if (detector_->has_detection()) {
                detector_->take_detection(out.detection);
                out.detected = true;
                if (!receiver_->start(dfe_start_from_detection(out.detection))) break;
            } else if (det >= audio.size()) {
                break;
            }
            continue;
        }
        const std::size_t n = std::min(chunk, audio.size() - rx);
        rx += receiver_->push_audio(audio.subspan(rx, n));
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
