// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Burst sizes, and a compile-time home for the header-only symbol assembly so the Cortex-M7
// build checks it, as scrambler_interleaver_check.cpp does for the scramblers.
#include "burst.hpp"

namespace m0lte::ms110d {

std::optional<BurstLayout> burst_layout(const TxSettings& config, std::size_t payload_bits) noexcept {
    const auto mode = mode_3k(config.waveform_number);
    if (!mode) return std::nullopt;
    const auto il = interleaver_3k(config.waveform_number, config.interleaver);
    if (!il) return std::nullopt;
    const auto long_il = interleaver_3k(config.waveform_number, InterleaverKind::Long);
    if (!long_il) return std::nullopt;
    if (config.constraint_length != 7 && config.constraint_length != 9) return std::nullopt;
    if (config.tlc_blocks < 0 || config.tlc_blocks > 255 || config.preamble_superframes < 1 || config.preamble_superframes > 32) {
        return std::nullopt;
    }

    BurstLayout l{};
    l.mode = *mode;
    l.interleaver_kind = config.interleaver;
    l.interleaver = *il;
    l.long_frames = long_il->frames;
    l.append_eot = config.append_eot;
    l.blocks = framed_block_count(payload_bits, config.append_eom, static_cast<std::size_t>(il->input_bits));
    l.framed_bits = l.blocks * static_cast<std::size_t>(il->input_bits);
    l.wire_bits = l.blocks * static_cast<std::size_t>(il->size_bits);
    l.preamble_symbols = Preamble::chip_count(config.tlc_blocks, config.preamble_superframes);
    const auto frames = static_cast<std::size_t>(il->frames);
    if (mode->wn == 0) {
        l.initial_probe_symbols = 0;
        l.data_symbols = l.blocks * frames * 32;
        l.eot_symbols = 0;  // no probes, so no EOT
    } else {
        l.initial_probe_symbols = static_cast<std::size_t>(mode->k);
        l.data_symbols = l.blocks * frames * static_cast<std::size_t>(mode->u);
        l.eot_symbols = config.append_eot ? 32 : 0;
    }
    const std::size_t probes = mode->wn == 0 ? 0 : l.blocks * frames * static_cast<std::size_t>(mode->k);
    l.total_symbols = l.preamble_symbols + l.initial_probe_symbols + l.data_symbols + probes + l.eot_symbols;
    return l;
}

namespace {

struct CountingSink {
    std::size_t count = 0;
    void psk(std::uint8_t) noexcept { ++count; }
    void qam(std::uint8_t) noexcept { ++count; }
    void probe(tables::Complex32) noexcept { ++count; }
};

}  // namespace

// Instantiates the assembly for the build check (never called).
bool burst_build_check(const BurstLayout& layout, const Preamble& preamble, std::span<const std::uint8_t> bits,
                       std::size_t& count) noexcept {
    CountingSink sink;
    const bool ok = assemble_burst(layout, preamble, bits, sink);
    count = sink.count;
    return ok;
}

}  // namespace m0lte::ms110d
