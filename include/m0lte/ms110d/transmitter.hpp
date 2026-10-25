// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The transmit path: payload bits in, 9600 Hz audio out, for one MIL-STD-188-110D Appendix D
// 3 kHz burst at a time. It does the same as the C# Ms110dModulator.Modulate (framing and EOM,
// tail-biting convolutional code, puncturing, interleaving, preamble, scrambling, mini-probes,
// EOT, pulse shaping, mixing to the 1800 Hz sub-carrier) and produces the same audio to within
// 1e-6 per sample, but as a stream: the audio is read in chunks of any size, down to one
// sample, so it can feed a sound card or a DMA buffer as it is made.
//
// Memory. The object allocates nothing, ever, and throws nothing. All its working memory is
// one block that the caller supplies to configure(): the settings and a largest payload (in
// bits) decide how big (storage_bytes()). The block can come from a vector, a static array or
// the stack. The transmitter keeps the payload bits until the audio has been read, so the block
// holds the payload too; reading takes no further memory.
//
// Use:
//     TxSettings settings;                                  // defaults as the C# Ms110dTxSettings
//     size_t bytes;  Transmitter::storage_bytes(settings, max_bits, bytes);
//     std::vector<uint8_t> memory(bytes);
//     Transmitter tx;  tx.configure(settings, max_bits, memory);
//     tx.push_bits(bits);  tx.end_payload();                // bits are 0 or 1 per byte
//     size_t n;  tx.audio_samples(bit_count, n);            // total length, known before reading
//     while (!tx.done()) { size_t got; tx.read(chunk, got); /* play chunk[0..got) */ }
// or, for a whole burst at once, tx.modulate(bits, audio, written).
//     tx.begin();                                           // ready for the next burst
// A burst is complete: preamble, data, EOM and EOT as the settings say. Not thread safe; use
// one object from one thread at a time.

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/status.hpp>
#include <m0lte/ms110d/tx_settings.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

// Sample rate of the audio the transmitter produces.
inline constexpr int tx_sample_rate_hz = 9600;

// Largest maximum payload accepted, in bits, so that sizes and sample counts fit 32 bits.
inline constexpr std::size_t max_payload_bits_limit = std::size_t{1} << 24;

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

class Transmitter {
public:
    Transmitter() noexcept = default;
    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;
    // Moving hands the transmitter, with its memory block, to the new object and leaves the
    // old one unconfigured.
    Transmitter(Transmitter&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
    Transmitter& operator=(Transmitter&& other) noexcept {
        if (this != &other) {
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    // Bytes of storage that configure() needs for these settings and a payload of up to
    // max_payload_bits bits (0 to max_payload_bits_limit). Fails with InvalidArgument for
    // settings out of range (waveform number, constraint length, preamble super-frames,
    // TLC blocks, roll-off, a non-finite amplitude) and Unsupported for a waveform and
    // interleaver pair the standard does not define (WN 0 has no ultra-short row).
    static Status storage_bytes(const TxSettings& settings, std::size_t max_payload_bits,
                                std::size_t& bytes) noexcept;

    // Validates the settings and builds the transmitter in `storage`, which must hold at least
    // storage_bytes() bytes (any alignment) and stay in place and unused by anything else for
    // as long as this object is used. BufferTooSmall if it is smaller. On success the
    // transmitter is ready for push_bits(); on failure it is left as it was.
    Status configure(const TxSettings& settings, std::size_t max_payload_bits,
                     std::span<std::uint8_t> storage) noexcept;
    bool configured() const noexcept { return impl_ != nullptr; }

    // Total audio samples (the burst plus the pulse tail) for a payload of payload_bits bits:
    // the same length as the C# Modulate result. State if not configured, InvalidArgument if
    // payload_bits is above max_payload_bits_limit. It does not depend on the bits themselves.
    Status audio_samples(std::size_t payload_bits, std::size_t& samples) const noexcept;

    // Starts a new burst: forgets the payload and any audio not yet read. configure() does this.
    Status begin() noexcept;

    // Appends payload bits, one per byte (only the low bit of each is used). BufferTooSmall,
    // taking none of them, if the payload would pass the maximum given to configure().
    // State once end_payload() has been called.
    Status push_bits(std::span<const std::uint8_t> bits) noexcept;

    // The payload is complete: the audio length is now fixed and read() can start. State if
    // already called. An empty payload is valid (the burst then carries only the EOM, if on).
    Status end_payload() noexcept;

    // The whole burst in one call, like the C# Modulate(bits): begin(), push_bits(bits),
    // end_payload() and reads everything into `audio`. `written` is the burst's length in
    // samples, always set when the payload fits: when `audio` is shorter the status is
    // BufferTooSmall, nothing is written to it and `written` is the length needed. A payload over
    // the maximum given to configure() is BufferTooSmall with `written` 0.
    Status modulate(std::span<const std::uint8_t> bits, std::span<float> audio, std::size_t& written) noexcept;

    // Writes up to audio.size() samples and sets `written` to how many (fewer only at the end of
    // the burst; 0 means the burst is finished, and only that). InvalidArgument if audio is
    // empty, State before end_payload(). The first read after a
    // block boundary of the data does the coding work for that block, so calls vary in cost;
    // the size of a chunk never changes the audio.
    Status read(std::span<float> audio, std::size_t& written) noexcept;

    // Samples still to be read after end_payload() (0 before it, and once done).
    std::size_t remaining() const noexcept;
    // True once end_payload() has been called and every sample has been read.
    bool done() const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // inline namespace

}  // namespace m0lte::ms110d
