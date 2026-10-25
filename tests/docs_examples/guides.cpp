// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The snippets in docs/transmitting.md and docs/receiving.md, compiled and run by ctest so they
// cannot rot. The regions between the snippet markers are checked to appear word for word in the
// guides.

#include <m0lte/ms110d/receiver.hpp>
#include <m0lte/ms110d/transmitter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace m0lte::ms110d;

static std::size_t g_played = 0;
static void play(const float* /*samples*/, std::size_t count) { g_played += count; }

// snippet-begin tx_static
// A transmitter in static memory, played out in 480 sample chunks (50 ms at 9600 Hz).
constexpr std::size_t max_bits = 2000;
alignas(8) static std::array<std::uint8_t, 16 * 1024> tx_memory;  // WN 4, short interleaver needs about 7.3 KiB

bool send(const std::vector<std::uint8_t>& payload_bits) {
    TxSettings settings;
    settings.waveform_number = 4;
    static Transmitter tx;
    if (tx.configure(settings, max_bits, tx_memory) != Status::Ok) return false;  // too small is BufferTooSmall
    if (tx.push_bits(payload_bits) != Status::Ok || tx.end_payload() != Status::Ok) return false;
    std::array<float, 480> chunk;
    while (!tx.done()) {
        std::size_t got = 0;
        if (tx.read(chunk, got) != Status::Ok) return false;
        play(chunk.data(), got);        // hand it to the sound card or the DMA buffer
    }
    return true;
}
// snippet-end tx_static

// snippet-begin mcu_rx
// A receiver for the ultra-short interleaver only, in a static array (no heap, no allocation).
// 1430848 bytes is what storage_bytes() says for these options in the default build; configure()
// answers BufferTooSmall if the array is too small, so a wrong figure is caught at start-up.
alignas(8) static std::array<std::uint8_t, 1430848> rx_memory;

bool start_receiver(Receiver& rx) {
    RxOptions options;
    options.max_interleaver = InterleaverKind::UltraShort;
    return rx.configure(options, rx_memory) == Status::Ok;
}
// snippet-end mcu_rx

// snippet-begin bytes_bits
// Bytes to bits, most significant bit first, one bit per byte; and back. Any order works as long
// as both ends use the same one.
std::vector<std::uint8_t> to_bits(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> bits;
    for (const std::uint8_t b : bytes)
        for (int i = 7; i >= 0; --i) bits.push_back(static_cast<std::uint8_t>((b >> i) & 1));
    return bits;
}

std::vector<std::uint8_t> to_bytes(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> bytes(bits.size() / 8);
    for (std::size_t i = 0; i < bytes.size() * 8; ++i)
        bytes[i / 8] = static_cast<std::uint8_t>((bytes[i / 8] << 1) | (bits[i] & 1));
    return bytes;
}
// snippet-end bytes_bits

static int g_started = 0, g_blocks = 0, g_completed = 0, g_refused = 0;

// snippet-begin rx_events
// Take every queued event. Call it after every process(), also one that used all its audio.
void drain(Receiver& rx) {
    for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
        switch (e.kind) {
        case RxEventKind::BurstStarted:
            std::printf("burst: WN %d, interleaver %d, K %d, %.1f Hz off\n",
                        e.waveform, static_cast<int>(e.interleaver), e.constraint_length, e.cfo_hz);
            ++g_started;
            break;
        case RxEventKind::BlockDecoded:
            ++g_blocks;                 // e.bits is valid until the next process() or reset()
            break;
        case RxEventKind::BurstCompleted:
            std::printf("done: %lld bits, %d blocks, reason %d\n",
                        static_cast<long long>(e.payload_bits), e.blocks, static_cast<int>(e.reason));
            ++g_completed;
            break;
        case RxEventKind::BurstRefused:
            std::printf("refused: WN %d needs a longer interleaver than this receiver holds\n", e.waveform);
            ++g_refused;
            break;
        default:
            break;                      // soft values, and kinds added later
        }
    }
}
// snippet-end rx_events

// snippet-begin rx_feed
// Feed one chunk of audio of any size. False if the receiver reported an error.
bool feed(Receiver& rx, std::span<const float> chunk) {
    std::size_t done = 0;
    while (done < chunk.size()) {
        std::size_t used = 0;
        if (rx.process(chunk.subspan(done), used) != Status::Ok) return false;
        done += used;
        drain(rx);
    }
    return true;
}
// snippet-end rx_feed

static std::vector<float> make_burst() {
    TxSettings settings;
    settings.waveform_number = 4;
    std::vector<std::uint8_t> bits(800, 1);
    std::size_t bytes = 0;
    Transmitter::storage_bytes(settings, bits.size(), bytes);
    std::vector<std::uint8_t> memory(bytes);
    Transmitter tx;
    tx.configure(settings, bits.size(), memory);
    std::size_t written = 0;
    tx.modulate(bits, {}, written);     // ask for the length
    std::vector<float> audio(written + 2 * 9600);
    tx.modulate(bits, std::span(audio).subspan(9600, written), written);
    return audio;
}

static bool receive_with(InterleaverKind longest, const std::vector<float>& audio) {
    RxOptions options;
    options.max_interleaver = longest;
    std::size_t bytes = 0;
    if (Receiver::storage_bytes(options, bytes) != Status::Ok) return false;
    std::vector<std::uint8_t> memory(bytes);
    Receiver rx;
    if (rx.configure(options, memory) != Status::Ok) return false;
    for (std::size_t at = 0; at < audio.size(); at += 480) {
        const std::size_t n = std::min<std::size_t>(480, audio.size() - at);
        if (!feed(rx, std::span(audio).subspan(at, n))) return false;
    }
    // snippet-begin rx_stats
    const RxStats stats = rx.stats();
    std::printf("%lld samples, %lld bursts refused, %lld turbo passes converged\n",
                static_cast<long long>(stats.samples_taken), static_cast<long long>(stats.bursts_refused),
                static_cast<long long>(stats.turbo_converged));
    // snippet-end rx_stats
    return true;
}

int main() {
    const std::vector<std::uint8_t> text = {0x48, 0x69, 0x80};
    if (to_bytes(to_bits(text)) != text) return 5;
    Receiver mcu;
    if (!start_receiver(mcu)) return 4;
#ifndef MS110D_SINGLE_PRECISION
    // The microcontroller guide quotes the exact figure: keep it exact.
    RxOptions ultra_short;
    ultra_short.max_interleaver = InterleaverKind::UltraShort;
    std::size_t ultra_short_bytes = 0;
    if (Receiver::storage_bytes(ultra_short, ultra_short_bytes) != Status::Ok || ultra_short_bytes != rx_memory.size())
        return 6;
#endif
    if (!send(std::vector<std::uint8_t>(800, 1)) || g_played == 0) return 1;

    const std::vector<float> audio = make_burst();
    if (!receive_with(InterleaverKind::Short, audio) || g_started != 1 || g_completed != 1 || g_blocks == 0) return 2;
    if (!receive_with(InterleaverKind::UltraShort, audio) || g_refused != 1) return 3;
    return 0;
}
