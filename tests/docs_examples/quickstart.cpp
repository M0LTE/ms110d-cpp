// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The C++ quick start from the README, compiled and run by ctest so it cannot rot. The regions
// between the snippet markers are checked to appear word for word in the README.

#include <m0lte/ms110d/receiver.hpp>
#include <m0lte/ms110d/transmitter.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace m0lte::ms110d;

// snippet-begin transmit
// Payload bits (one per byte, 0 or 1) in, 9600 Hz audio out.
std::vector<float> transmit(const std::vector<std::uint8_t>& payload_bits) {
    TxSettings settings;                 // WN 6, short interleaver, K 7, and so on
    settings.waveform_number = 4;        // 1200 bps BPSK

    const std::size_t max_bits = 4000;   // the largest payload this transmitter will carry
    std::size_t bytes = 0;
    if (Transmitter::storage_bytes(settings, max_bits, bytes) != Status::Ok) return {};
    std::vector<std::uint8_t> memory(bytes);  // or a static array: the library never allocates

    Transmitter tx;
    if (tx.configure(settings, max_bits, memory) != Status::Ok) return {};
    if (tx.push_bits(payload_bits) != Status::Ok) return {};  // more than max_bits in all
    if (tx.end_payload() != Status::Ok) return {};

    std::size_t total = 0;
    tx.audio_samples(payload_bits.size(), total);  // the whole burst's length, known up front
    std::vector<float> audio(total);
    std::size_t have = 0;
    while (!tx.done()) {
        std::size_t got = 0;
        if (tx.read(std::span(audio).subspan(have), got) != Status::Ok) return {};  // any chunk size
        have += got;
    }
    return audio;
}
// snippet-end transmit

// snippet-begin receive
// 9600 Hz audio in, the first decoded burst's payload bits out (one per byte).
std::vector<std::uint8_t> receive(std::span<const float> audio) {
    RxOptions options;                   // the same defaults as the C# demodulator
    options.max_interleaver = InterleaverKind::Short;  // 4.1 MiB; Long accepts everything in 58.2 MiB
    std::size_t bytes = 0;
    if (Receiver::storage_bytes(options, bytes) != Status::Ok) return {};
    std::vector<std::uint8_t> memory(bytes);  // or a static array: the library never allocates

    Receiver rx;
    if (rx.configure(options, memory) != Status::Ok) return {};

    std::vector<std::uint8_t> payload;
    std::size_t done = 0;
    while (done < audio.size()) {
        std::size_t used = 0;
        if (rx.process(audio.subspan(done), used) != Status::Ok) break;
        done += used;                    // it stops right after a sample that raised events
        for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
            if (e.kind == RxEventKind::BurstStarted) payload.clear();  // e.waveform, e.interleaver, ...
            if (e.kind == RxEventKind::BlockDecoded) payload.insert(payload.end(), e.bits.begin(), e.bits.end());
            if (e.kind == RxEventKind::BurstCompleted) {
                payload.resize(static_cast<std::size_t>(e.payload_bits));  // the bits before the EOM
                return payload;
            }
        }
    }
    return {};
}
// snippet-end receive

int main() {
    std::vector<std::uint8_t> bits(1000);
    for (std::size_t i = 0; i < bits.size(); ++i) bits[i] = static_cast<std::uint8_t>((i * 7 + i / 5) & 1);

    std::vector<float> audio = transmit(bits);
    if (audio.empty()) return 1;
    // A second of silence either side, as a real channel would have.
    audio.insert(audio.begin(), 9600, 0.0f);
    audio.insert(audio.end(), 9600, 0.0f);

    const std::vector<std::uint8_t> got = receive(audio);
    if (got != bits) {
        std::fprintf(stderr, "quickstart: sent %zu bits, got %zu back\n", bits.size(), got.size());
        return 2;
    }
    return 0;
}
