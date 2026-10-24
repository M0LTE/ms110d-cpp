// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Appendix D autobaud preamble (D.5.2.1): a TLC section of N 32-chip blocks, then M
// super-frames of [Fixed][4 downcount di-bits][5 waveform ID di-bits], each di-bit a 4-ary
// Walsh channel symbol of 32 chips, scrambled modulo 8 with fixedPN, cntPN or widPN. Ported
// from the C# PreambleGenerator, including its reading of the Fixed section (fixedPN
// indexed modulo 256 across the 288 chips; at 3 kHz that equals restarting per symbol).
//
// A Preamble holds a handful of integers and the five WID di-bits; any chip is computed on
// demand from its index, so nothing is buffered and nothing allocates.

#include <m0lte/ms110d/mode.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace m0lte::ms110d {

class Preamble {
public:
    // N TLC blocks (0 to 255) and M super-frames (1 to 32) for a waveform configuration.
    // Nothing is returned when a number is out of range (see encode_wid for the waveform).
    static std::optional<Preamble> make(int tlc_blocks, int superframes, int wn, InterleaverKind interleaver,
                                        int constraint_length) noexcept;

    // Chips in a preamble, without building one: the TLC section plus M super-frames.
    static constexpr std::size_t chip_count(int tlc_blocks, int superframes) noexcept {
        return static_cast<std::size_t>(chips_per_symbol) *
               (static_cast<std::size_t>(tlc_blocks) +
                static_cast<std::size_t>(superframes) * (superframes == 1 ? 10u : 18u));
    }

    std::size_t chip_count() const noexcept { return chip_count(tlc_, m_); }

    // 8PSK chip number (0 to 7) at `index`, which must be below chip_count().
    std::uint8_t chip(std::size_t index) const noexcept;

    // Writes every chip; `out` must hold at least chip_count().
    void write(std::span<std::uint8_t> out) const noexcept;

    static constexpr int chips_per_symbol = 32;

private:
    Preamble(int tlc, int m, std::array<std::uint8_t, 5> wid) noexcept : tlc_(tlc), m_(m), wid_(wid) {}

    int tlc_;
    int m_;
    std::array<std::uint8_t, 5> wid_;
};

}  // namespace m0lte::ms110d
