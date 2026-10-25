// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mask_driver.hpp"

#include "tables.hpp"

#include <algorithm>
#include <stdexcept>

namespace ms110d_test {

using namespace m0lte::ms110d;

namespace {
constexpr std::size_t chunk = 2400;
}

MaskDriver::MaskDriver()
    : detector_(std::make_unique<PreambleDetector>()),
      dfe_(std::make_unique<DfeReceiver>()),
      wn0_(std::make_unique<Wn0Receiver>()) {
    DfeReceiverConfig config;
    config.max_interleaver = InterleaverKind::Long;
    const std::size_t bits = DfeReceiver::max_input_bits_for(InterleaverKind::Long);
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
    if (!dfe_->configure(config, storage_, d)) throw std::runtime_error("DfeReceiver configure failed");
    if (!wn0_->configure(default_roll_off)) throw std::runtime_error("Wn0Receiver configure failed");
    // The longest WN 0 block: Table D-XXXVII long, 1152 coded bits, 576 info bits.
    wn0_k7_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k7_133_171, 576);
    wn0_k9_ = std::make_unique<m0lte::fec::TailBitingViterbiDecoder>(m0lte::fec::k9_561_753, 576);
    wn0_block_.resize(1152);
}

MaskBurst MaskDriver::run(std::span<const float> audio) {
    MaskBurst out;
    detector_->reset();
    // Searching. The receivers' own front ends are fed the same audio from the same first sample.
    std::size_t det = 0;
    while (det < audio.size() && !detector_->has_detection()) {
        det += detector_->process(audio.subspan(det, std::min(chunk, audio.size() - det)));
    }
    if (!detector_->has_detection()) return out;
    detector_->take_detection(detection_);
    out.detected = true;
    out.waveform = detection_.waveform;
    if (detection_.waveform == 0) {
        run_wn0(audio, det, out);
    } else {
        run_dfe(audio, det, out);
    }
    return out;
}

namespace {

// The C# FindEom over the burst's bits so far: true when the 32 bit EOM word ends inside the
// last `fresh` bits. `window` carries the previous bits between calls.
bool scan_eom(std::uint32_t& window, std::int64_t& seen, std::span<const std::uint8_t> block) {
    for (const std::uint8_t b : block) {
        window = (window << 1) | b;
        ++seen;
        if (seen >= 32 && window == tables::eom) return true;
    }
    return false;
}

}  // namespace

void MaskDriver::run_wn0(std::span<const float> audio, std::size_t used, MaskBurst& out) {
    wn0_->reset();
    const auto params = interleaver_3k(0, detection_.interleaver);
    if (!params) return;
    const Interleaver il(static_cast<std::uint32_t>(params->size_bits), static_cast<std::uint32_t>(params->increment));
    Wn0Start s;
    s.chip0 = detection_.chip0_t2;
    s.tau = 0;
    s.omega = detection_.carrier_omega;
    s.theta_base = detection_.carrier_theta_base;
    s.data_start_chip = static_cast<std::int64_t>(detection_.superframes_after + 1) * 576;
    s.interleaver = il;
    // Everything the detector has used, then the burst, in chunks with a step between each.
    std::size_t at = 0;
    while (at < used) {
        const std::size_t n = std::min<std::size_t>(chunk, used - at);
        wn0_->push_audio(audio.subspan(at, n));
        at += n;
    }
    if (!wn0_->start(s, wn0_block_)) return;
    auto& dec = detection_.constraint_length == 9 ? *wn0_k9_ : *wn0_k7_;
    std::vector<std::uint8_t> info(il.size_bits() / 2);
    std::uint32_t window = 0;
    std::int64_t seen = 0;
    bool more = true;
    while (more) {
        for (;;) {
            const Wn0Step st = wn0_->poll();
            if (st == Wn0Step::BlockComplete) {
                if (!wn0_->decode_block(dec, info)) return;
                out.bits.insert(out.bits.end(), info.begin(), info.end());
                if (scan_eom(window, seen, info)) {
                    out.eom = true;
                    return;
                }
                continue;
            }
            if (st == Wn0Step::SignalLost || st == Wn0Step::Inactive) return;
            break;  // NeedMoreAudio
        }
        if (at >= audio.size()) break;
        const std::size_t n = std::min<std::size_t>(chunk, audio.size() - at);
        wn0_->push_audio(audio.subspan(at, n));
        at += n;
    }
}

void MaskDriver::run_dfe(std::span<const float> audio, std::size_t used, MaskBurst& out) {
    dfe_->reset();
    std::size_t rx = 0;
    while (rx < used) rx += dfe_->push_audio(audio.subspan(rx, used - rx));
    if (!dfe_->start(dfe_start_from_detection(detection_))) return;
    while (rx < audio.size()) {
        rx += dfe_->push_audio(audio.subspan(rx, std::min(chunk, audio.size() - rx)));
        while (dfe_->event() != DfeEvent::None) {
            if (dfe_->event() == DfeEvent::BlockDecoded) {
                const DfeBlock& b = dfe_->block();
                out.bits.insert(out.bits.end(), b.info.begin(), b.info.end());
            } else {
                out.eom = dfe_->burst_end().reason == DfeEndReason::Eom;
                dfe_->take_event();
                return;
            }
            dfe_->take_event();
        }
    }
}

}  // namespace ms110d_test
