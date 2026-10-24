// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rx_driver.hpp"

#include <algorithm>
#include <stdexcept>

namespace ms110d_test {

using namespace m0lte::ms110d;

RxDriver::RxDriver(InterleaverKind max_interleaver, bool trace_frames)
    : detector_(std::make_unique<PreambleDetector>()),
      receiver_(std::make_unique<DfeReceiver>()),
      trace_frames_(trace_frames) {
    DfeReceiverConfig config;
    config.max_interleaver = max_interleaver;
    const std::size_t bits = DfeReceiver::max_input_bits_for(max_interleaver);
    k7_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k7_133_171, bits);
    k9_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k9_561_753, bits);
    storage_.resize(DfeReceiver::storage_bytes(config));
    if (!receiver_->configure(config, storage_, k7_.get(), k9_.get())) throw std::runtime_error("configure failed");
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
                out.blocks.push_back({b.index, b.at_t2, {b.info.begin(), b.info.end()}, {b.llrs.begin(), b.llrs.end()}});
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
