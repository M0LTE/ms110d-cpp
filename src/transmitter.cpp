// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <m0lte/ms110d/transmitter.hpp>

#include "burst.hpp"
#include "fec/convolutional.hpp"
#include "framing.hpp"
#include "interleaver.hpp"
#include "modulator.hpp"
#include "preamble.hpp"
#include "puncture.hpp"
#include "tables.hpp"
#include "trinomial_scrambler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

// The longest run of symbols made at once: a frame of 256 data symbols and a 32 symbol probe
// with the 32 symbol EOT marker (the preamble goes out 32 chips at a time, plus the 48 symbol
// initial probe).
constexpr std::size_t symbol_buffer_size = 320;
constexpr std::size_t read_chunk = 32;

struct SymbolBuffer {
    tables::Complex32* out;
    std::size_t count = 0;
    void push(const tables::Complex32& v) noexcept {
        assert(count < symbol_buffer_size);
        out[count++] = v;
    }
    void psk(std::uint8_t s) noexcept { push(tables::psk8[s]); }
    void qam(std::uint8_t s) noexcept { push(tables::qam16[s]); }
    void probe(tables::Complex32 v) noexcept { push(v); }
};

// What the settings and a maximum payload come to, shared by sizing and configuring.
struct Plan {
    BurstLayout layout;  // for an empty payload; only the mode and interleaver are used from it
    fec::ConvolutionalCode code;
    const PunctureSpec* puncture;
    std::size_t work1;  // framed bits, then punctured bits
    std::size_t work2;  // coded bits, then interleaved (wire) bits
};

Status plan_for(const TxSettings& s, std::size_t max_payload_bits, Plan& plan) noexcept {
    if (max_payload_bits > max_payload_bits_limit) return Status::InvalidArgument;
    if (s.constraint_length != 7 && s.constraint_length != 9) return Status::InvalidArgument;
    if (s.tlc_blocks < 0 || s.tlc_blocks > 255 || s.preamble_superframes < 1 || s.preamble_superframes > 32) {
        return Status::InvalidArgument;
    }
    if (!valid_roll_off(s.roll_off) || !std::isfinite(s.amplitude)) return Status::InvalidArgument;
    const auto mode = mode_3k(s.waveform_number);
    if (!mode) return Status::InvalidArgument;
    const int kind = static_cast<int>(s.interleaver);
    if (kind < 0 || kind > 3) return Status::InvalidArgument;
    if (!has_interleaver_3k(s.waveform_number, s.interleaver)) return Status::Unsupported;
    const auto layout = burst_layout(s, 0);
    if (!layout) return Status::InvalidArgument;
    plan.layout = *layout;
    plan.code = s.constraint_length == 9 ? fec::k9_561_753 : fec::k7_133_171;
    plan.puncture = find_puncture(plan.code, static_cast<unsigned>(mode->code_rate.numerator),
                                  static_cast<unsigned>(mode->code_rate.denominator));
    const auto input = static_cast<std::size_t>(layout->interleaver.input_bits);
    const auto size = static_cast<std::size_t>(layout->interleaver.size_bits);
    if (plan.puncture == nullptr) return Status::Unsupported;
    const auto punctured = punctured_length(*plan.puncture, input);
    if (!punctured || *punctured != size) return Status::Unsupported;  // a table bug, not a caller error
    plan.work1 = std::max(input, size);
    plan.work2 = std::max(2 * input, size);
    return Status::Ok;
}

}  // namespace

struct Transmitter::Impl {
    Impl(const TxSettings& s, const Plan& p, const Preamble& pre, std::size_t max_bits, std::uint8_t* w1,
         std::uint8_t* w2, std::uint8_t* payload_bits) noexcept
        : settings(s),
          layout(p.layout),
          preamble(pre),
          code(p.code),
          puncture(p.puncture),
          interleaver(static_cast<std::uint32_t>(p.layout.interleaver.size_bits),
                      static_cast<std::uint32_t>(p.layout.interleaver.increment)),
          work1(w1),
          work2(w2),
          payload(payload_bits),
          max_payload(max_bits) {}

    TxSettings settings;
    BurstLayout layout;  // for the payload length once end_payload() has been called
    Preamble preamble;
    fec::ConvolutionalCode code;
    const PunctureSpec* puncture;
    Interleaver interleaver;
    std::uint8_t* work1;
    std::uint8_t* work2;
    std::uint8_t* payload;
    std::size_t max_payload;
    Modulator modulator;
    Trinomial159Scrambler walsh;
    std::array<tables::Complex32, symbol_buffer_size> symbols;
    std::size_t symbol_count = 0;
    std::size_t symbol_next = 0;
    std::size_t payload_len = 0;
    std::size_t total_samples = 0;
    std::size_t emitted = 0;
    std::size_t preamble_pos = 0;
    std::size_t block = 0;
    std::size_t unit = 0;
    std::size_t units_per_block = 0;
    bool streaming = false;
    bool symbols_ended = false;

    std::span<const std::uint8_t> wire() const noexcept {
        return {work2, static_cast<std::size_t>(layout.interleaver.size_bits)};
    }

    void reset() noexcept {
        payload_len = 0;
        total_samples = 0;
        emitted = 0;
        preamble_pos = 0;
        block = 0;
        unit = 0;
        symbol_count = 0;
        symbol_next = 0;
        streaming = false;
        symbols_ended = false;
    }

    // Frames, codes, punctures and interleaves one block into work2.
    void prepare_block(std::size_t b) noexcept {
        const auto input = static_cast<std::size_t>(layout.interleaver.input_bits);
        const auto size = static_cast<std::size_t>(layout.interleaver.size_bits);
        const std::span<std::uint8_t> framed(work1, input);
        const std::span<std::uint8_t> coded(work2, 2 * input);
        const std::span<std::uint8_t> punctured(work1, size);
        [[maybe_unused]] const bool framed_ok =
            build_tx_block({payload, payload_len}, settings.append_eom, input, b, framed);
        [[maybe_unused]] const bool coded_ok = fec::encode_tail_biting(code, framed, coded);
        [[maybe_unused]] const auto n = apply_puncture(*puncture, coded, punctured);
        assert(framed_ok && coded_ok && n && *n == size);
        interleaver.interleave(work1, work2);
    }

    // Makes the next run of symbols; leaves the buffer empty when the burst is complete.
    void refill() noexcept {
        symbol_count = 0;
        symbol_next = 0;
        SymbolBuffer sink{symbols.data()};
        if (preamble_pos < layout.preamble_symbols) {
            const std::size_t n = std::min<std::size_t>(Preamble::chips_per_symbol, layout.preamble_symbols - preamble_pos);
            for (std::size_t i = 0; i < n; ++i) sink.psk(preamble.chip(preamble_pos + i));
            preamble_pos += n;
            if (preamble_pos == layout.preamble_symbols && layout.mode.wn != 0) write_probe(layout.mode.k, false, 0, sink);
        } else if (block < layout.blocks) {
            if (unit == 0) {
                prepare_block(block);
                walsh.reset();
            }
            if (layout.mode.wn == 0) {
                write_walsh_symbol(wire(), unit, walsh, sink);
            } else {
                write_frame(layout, block, static_cast<int>(unit), wire(), sink);
            }
            if (++unit == units_per_block) {
                unit = 0;
                ++block;
            }
        }
        symbol_count = sink.count;
    }
};

Status Transmitter::storage_bytes(const TxSettings& settings, std::size_t max_payload_bits, std::size_t& bytes) noexcept {
    Plan plan;
    const Status st = plan_for(settings, max_payload_bits, plan);
    if (st != Status::Ok) return st;
    bytes = sizeof(Impl) + alignof(Impl) - 1 + plan.work1 + plan.work2 + max_payload_bits;
    return Status::Ok;
}

Status Transmitter::configure(const TxSettings& settings, std::size_t max_payload_bits,
                              std::span<std::uint8_t> storage) noexcept {
    static_assert(std::is_trivially_destructible_v<Impl>, "the object lives in caller memory and is never destroyed");
    Plan plan;
    Status st = plan_for(settings, max_payload_bits, plan);
    if (st != Status::Ok) return st;
    const auto preamble = Preamble::make(settings.tlc_blocks, settings.preamble_superframes, settings.waveform_number,
                                         settings.interleaver, settings.constraint_length);
    if (!preamble) return Status::InvalidArgument;

    void* where = storage.data();
    std::size_t space = storage.size();
    const std::size_t need = plan.work1 + plan.work2 + max_payload_bits;
    if (std::align(alignof(Impl), sizeof(Impl), where, space) == nullptr || space < sizeof(Impl) + need) {
        return Status::BufferTooSmall;
    }
    auto* bytes = static_cast<std::uint8_t*>(where) + sizeof(Impl);
    auto* impl = ::new (where) Impl(settings, plan, *preamble, max_payload_bits, bytes, bytes + plan.work1,
                                    bytes + plan.work1 + plan.work2);
    if (!impl->modulator.configure(settings)) return Status::InvalidArgument;
    impl->units_per_block = settings.waveform_number == 0 ? static_cast<std::size_t>(plan.layout.interleaver.size_bits) / 2
                                                          : static_cast<std::size_t>(plan.layout.interleaver.frames);
    impl_ = impl;
    impl_->reset();
    return Status::Ok;
}

Status Transmitter::audio_samples(std::size_t payload_bits, std::size_t& samples) const noexcept {
    if (impl_ == nullptr) return Status::State;
    if (payload_bits > max_payload_bits_limit) return Status::InvalidArgument;
    const auto layout = burst_layout(impl_->settings, payload_bits);
    if (!layout) return Status::InvalidArgument;
    samples = audio_length(layout->total_symbols, impl_->modulator.taps());
    return Status::Ok;
}

Status Transmitter::begin() noexcept {
    if (impl_ == nullptr) return Status::State;
    impl_->reset();
    return Status::Ok;
}

Status Transmitter::push_bits(std::span<const std::uint8_t> bits) noexcept {
    if (impl_ == nullptr || impl_->streaming) return Status::State;
    if (bits.size() > impl_->max_payload - impl_->payload_len) return Status::BufferTooSmall;
    for (const std::uint8_t b : bits) impl_->payload[impl_->payload_len++] = b & 1u;
    return Status::Ok;
}

Status Transmitter::end_payload() noexcept {
    if (impl_ == nullptr || impl_->streaming) return Status::State;
    Impl& m = *impl_;
    const auto layout = burst_layout(m.settings, m.payload_len);
    if (!layout) return Status::InvalidArgument;
    m.layout = *layout;
    m.total_samples = audio_length(layout->total_symbols, m.modulator.taps());
    m.emitted = 0;
    m.preamble_pos = 0;
    m.block = 0;
    m.unit = 0;
    m.symbol_count = 0;
    m.symbol_next = 0;
    m.symbols_ended = false;
    m.modulator.begin();
    m.streaming = true;
    return Status::Ok;
}

Status Transmitter::read(std::span<float> audio, std::size_t& written) noexcept {
    written = 0;
    if (impl_ == nullptr || !impl_->streaming) return Status::State;
    Impl& m = *impl_;
    real chunk[read_chunk];
    while (written < audio.size()) {
        if (!m.symbols_ended && m.modulator.pending() == 0) {
            if (m.symbol_next == m.symbol_count) m.refill();
            if (m.symbol_next == m.symbol_count) {
                m.modulator.end_of_symbols();
                m.symbols_ended = true;
            } else {
                m.modulator.push_symbol(m.symbols[m.symbol_next++]);
            }
        }
        const std::size_t want = std::min(audio.size() - written, read_chunk);
        const std::size_t got = m.modulator.read(std::span<real>(chunk, want));
        if (got == 0) break;  // only once the tail is drained
        for (std::size_t i = 0; i < got; ++i) audio[written + i] = static_cast<float>(chunk[i]);
        written += got;
        m.emitted += got;
    }
    return Status::Ok;
}

std::size_t Transmitter::remaining() const noexcept {
    if (impl_ == nullptr || !impl_->streaming) return 0;
    return impl_->total_samples - impl_->emitted;
}

bool Transmitter::done() const noexcept {
    return impl_ != nullptr && impl_->streaming && impl_->emitted == impl_->total_samples;
}

}  // inline namespace

}  // namespace m0lte::ms110d
