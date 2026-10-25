// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mask_driver.hpp"

#include <algorithm>
#include <stdexcept>

namespace ms110d_test {

using namespace m0lte::ms110d;

MaskDriver::MaskDriver() {
    const RxOptions options;  // the C# defaults, Long interleaver accepted
    std::size_t bytes = 0;
    if (Receiver::storage_bytes(options, bytes) != Status::Ok) throw std::runtime_error("storage_bytes failed");
    memory_.resize(bytes);
    if (rx_.configure(options, memory_) != Status::Ok) throw std::runtime_error("configure failed");
}

MaskBurst MaskDriver::run(std::span<const float> audio) {
    MaskBurst out;
    rx_.reset();
    constexpr std::size_t chunk = 2400;
    for (std::size_t at = 0; at < audio.size();) {
        const std::span<const float> part = audio.subspan(at, std::min(chunk, audio.size() - at));
        std::size_t done = 0;
        while (done < part.size()) {
            std::size_t used = 0;
            rx_.process(part.subspan(done), used);
            done += used;
            for (RxEvent e = rx_.next_event(); e.kind != RxEventKind::None; e = rx_.next_event()) {
                if (e.kind == RxEventKind::BurstStarted) {
                    out.detected = true;
                    out.waveform = e.waveform;
                } else if (e.kind == RxEventKind::BlockDecoded) {
                    out.bits.insert(out.bits.end(), e.bits.begin(), e.bits.end());
                } else if (e.kind == RxEventKind::BurstCompleted) {
                    out.eom = e.reason == BurstEndReason::Eom;
                    return out;
                }
            }
        }
        at += part.size();
    }
    return out;
}

}  // namespace ms110d_test
