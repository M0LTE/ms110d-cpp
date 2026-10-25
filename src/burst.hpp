// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The whole burst's symbol stream, before pulse shaping (D.5.2, D.5.2.2, D.5.4.4), as the C#
// Ms110dModulator.BuildSymbols builds it: preamble, then for WN 0 the Walsh channel symbols of
// each interleaver block, otherwise an initial mini-probe and per frame U data symbols plus a
// mini-probe, the probe before a Long interleaver boundary cyclically shifted (D.5.2.2), and the
// last probe extended by 32 symbols as the EOT marker. Which probes are shifted is the standard
// rule, whatever interleaver is selected: see probe_is_shifted below and issue 68.
//
// The input is the wire-order (interleaved) bits of each block, as produced by the FEC stage,
// one bit per byte. Output goes to a Sink, so a caller can write bytes, feed a pulse shaper or
// just count, with no buffer here. A Sink provides
//     void psk(std::uint8_t symbol);                      // 8PSK symbol number 0..7
//     void qam(std::uint8_t symbol);                      // 16-QAM symbol number 0..15
//     void probe(tables::Complex32 value);                // one mini-probe symbol
// Blocks are independent (every scrambler resets per frame or per block), so a transmitter can
// call write_block one block at a time as it encodes them. The writers are noexcept and call
// the sink from inside them, so a sink must not throw.

#include "framing.hpp"
#include "preamble.hpp"
#include "scrambler.hpp"
#include "tables.hpp"
#include "trinomial_scrambler.hpp"
#include <m0lte/ms110d/tx_settings.hpp>

#include <m0lte/ms110d/mode.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace m0lte::ms110d {

// Sizes of a burst for a payload length, from the tables alone.
struct BurstLayout {
    Mode mode;
    InterleaverKind interleaver_kind;
    InterleaverParams interleaver;
    int long_frames;               // frames of the waveform's Long interleaver, the shift period
    bool append_eot;
    std::size_t blocks;            // interleaver blocks after EOM and zero fill
    std::size_t framed_bits;       // blocks * interleaver.input_bits
    std::size_t wire_bits;         // blocks * interleaver.size_bits, the input to write_block overall
    std::size_t preamble_symbols;
    std::size_t initial_probe_symbols;  // 0 for WN 0
    std::size_t data_symbols;      // WN 0: 32-chip Walsh symbols' chips; else unknown symbols
    std::size_t eot_symbols;       // 32 when an EOT follows the last probe, else 0
    std::size_t total_symbols;
};

// Nothing is returned for a waveform or interleaver the library does not define, a constraint
// length other than 7 or 9, or preamble settings out of range.
std::optional<BurstLayout> burst_layout(const TxSettings& config, std::size_t payload_bits) noexcept;

// Whether the probe after data frame g (numbered from 0 across the whole burst) is the shifted
// one. This is the D.5.2.2 standard rule: shifted only when g mod L == L - 2, L the frame count
// of the waveform's LONG interleaver, whatever interleaver is selected. The probe that ends the
// preamble is never shifted, and the EOT is a cyclic extension of the last probe as sent. The
// C# 0.2.1 transmitter instead shifts at the selected interleaver's boundary, (g + 2) % F == 0;
// the two coincide for the Long interleaver (issue 68).
constexpr bool probe_is_shifted(std::int64_t frame_number, int long_frames) noexcept {
    return frame_number % long_frames == long_frames - 2;
}

// One mini-probe of k symbols, shifted or not, followed by `extra` further symbols of the same
// cyclic extension (the EOT marker is 32).
template <class Sink>
void write_probe(int k, bool shifted, int extra, Sink& sink) noexcept {
    const tables::ProbeSpec* spec = tables::find_probe_spec(k);
    if (spec == nullptr) return;
    for (int i = 0; i < k + extra; ++i) sink.probe(tables::probe_symbol(*spec, i, shifted));
}

// The preamble, then for WN 1 and up the initial (unshifted) mini-probe.
template <class Sink>
void write_head(const BurstLayout& layout, const Preamble& preamble, Sink& sink) noexcept {
    // The preamble and the layout must describe the same waveform (a debug check only).
    assert(preamble.chip_count() == layout.preamble_symbols);
    assert(decode_wid(preamble.wid()).has_value() && decode_wid(preamble.wid())->wn == layout.mode.wn &&
           decode_wid(preamble.wid())->interleaver == layout.interleaver_kind);
    const std::size_t n = preamble.chip_count();
    for (std::size_t i = 0; i < n; ++i) sink.psk(preamble.chip(i));
    if (layout.mode.wn != 0) write_probe(layout.mode.k, false, 0, sink);
}

// One 32-chip Walsh channel symbol of a WN 0 block: the `index`-th pair of fetched bits (two
// bits per symbol, first bit the MSB of the di-bit). The scramble sequence restarts at the
// interleaver boundary (D.5.1.4), so the caller resets `scrambler` at the start of each block
// and calls this for index 0, 1, 2 ... in order. `wire_bits` is the whole block.
template <class Sink>
void write_walsh_symbol(std::span<const std::uint8_t> wire_bits, std::size_t index,
                        Trinomial159Scrambler& scrambler, Sink& sink) noexcept {
    const std::size_t n = index * 2;
    assert(n + 1 < wire_bits.size());
    const unsigned dibit = (static_cast<unsigned>(wire_bits[n] & 1u) << 1) | (wire_bits[n + 1] & 1u);
    for (int i = 0; i < 32; ++i) {
        sink.psk(static_cast<std::uint8_t>((tables::walsh[dibit][static_cast<std::size_t>(i) & 3] + scrambler.next()) & 7));
    }
}

// One data frame of a WN 1 and up block: U unknown symbols, scrambled afresh (D.5.1.3), then
// the mini-probe (shifted before a Long interleaver boundary), extended by the 32 symbol EOT
// marker after the very last frame of the burst. Frames are independent, so a transmitter can
// produce a burst a frame at a time. `wire_bits` is the whole block (layout.interleaver.size_bits
// bits), `frame` is 0 to layout.interleaver.frames - 1 and block_index below layout.blocks;
// the caller checks these (a debug build asserts).
template <class Sink>
void write_frame(const BurstLayout& layout, std::size_t block_index, int frame,
                 std::span<const std::uint8_t> wire_bits, Sink& sink) noexcept {
    assert(layout.mode.wn != 0 && block_index < layout.blocks && frame >= 0 && frame < layout.interleaver.frames);
    assert(wire_bits.size() == static_cast<std::size_t>(layout.interleaver.size_bits));
    const Mode& mode = layout.mode;
    const int bps = mode.bits_per_symbol;
    const auto bit = [&](std::size_t p) { return static_cast<unsigned>(wire_bits[p] & 1u); };
    std::size_t pos = static_cast<std::size_t>(frame) * static_cast<std::size_t>(mode.u) * static_cast<std::size_t>(bps);
    Scrambler scrambler;  // initialised to 1 at the start of each data frame (D.5.1.3)
    for (int u = 0; u < mode.u; ++u) {
        unsigned value = 0;
        for (int b = 0; b < bps; ++b) value = (value << 1) | bit(pos++);  // first fetched bit is the MSB
        switch (mode.modulation) {
            case Modulation::Bpsk:
                sink.psk(static_cast<std::uint8_t>(scrambler.next_psk(tables::transcode_bpsk[value])));
                break;
            case Modulation::Qpsk:
                sink.psk(static_cast<std::uint8_t>(scrambler.next_psk(tables::transcode_qpsk[value])));
                break;
            case Modulation::Psk8:
                sink.psk(static_cast<std::uint8_t>(scrambler.next_psk(tables::transcode_8psk[value])));
                break;
            case Modulation::Qam16:
                sink.qam(static_cast<std::uint8_t>(scrambler.next_qam(static_cast<int>(value), 4)));
                break;
            case Modulation::Walsh:
                break;
        }
    }
    const bool last = block_index + 1 == layout.blocks && frame == layout.interleaver.frames - 1;
    const std::int64_t frame_number = static_cast<std::int64_t>(block_index) * layout.interleaver.frames + frame;
    write_probe(mode.k, probe_is_shifted(frame_number, layout.long_frames), last && layout.append_eot ? 32 : 0, sink);
}

// Symbols of one interleaver block: `wire_bits` is exactly layout.interleaver.size_bits bits.
// Returns false and writes nothing if the length is wrong or block_index is out of range.
template <class Sink>
bool write_block(const BurstLayout& layout, std::size_t block_index, std::span<const std::uint8_t> wire_bits,
                 Sink& sink) noexcept {
    const auto size = static_cast<std::size_t>(layout.interleaver.size_bits);
    if (wire_bits.size() != size || block_index >= layout.blocks) return false;

    if (layout.mode.wn == 0) {
        Trinomial159Scrambler scrambler;
        for (std::size_t n = 0; n < size / 2; ++n) write_walsh_symbol(wire_bits, n, scrambler, sink);
        return true;
    }
    for (int f = 0; f < layout.interleaver.frames; ++f) write_frame(layout, block_index, f, wire_bits, sink);
    return true;
}

// Everything at once: `wire_bits` holds every block, layout.wire_bits long.
template <class Sink>
bool assemble_burst(const BurstLayout& layout, const Preamble& preamble, std::span<const std::uint8_t> wire_bits,
                    Sink& sink) noexcept {
    if (wire_bits.size() != layout.wire_bits) return false;
    write_head(layout, preamble, sink);
    const auto size = static_cast<std::size_t>(layout.interleaver.size_bits);
    for (std::size_t b = 0; b < layout.blocks; ++b) {
        if (!write_block(layout, b, wire_bits.subspan(b * size, size), sink)) return false;
    }
    return true;
}

}  // namespace m0lte::ms110d
