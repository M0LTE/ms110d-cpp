// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "preamble.hpp"

#include "framing.hpp"
#include "tables.hpp"

namespace m0lte::ms110d {

std::optional<Preamble> Preamble::make(int tlc_blocks, int superframes, int wn, InterleaverKind interleaver,
                                       int constraint_length) noexcept {
    if (tlc_blocks < 0 || tlc_blocks > 255 || superframes < 1 || superframes > 32) return std::nullopt;
    const auto wid = encode_wid(wn, interleaver, constraint_length);
    if (!wid) return std::nullopt;
    return Preamble(tlc_blocks, superframes, *wid);
}

std::uint8_t Preamble::chip(std::size_t index) const noexcept {
    constexpr std::size_t cps = chips_per_symbol;
    const std::size_t tlc_chips = static_cast<std::size_t>(tlc_) * cps;
    if (index < tlc_chips) {
        // D.5.2.1.2: the complex conjugate of the Fixed sequence; conjugate of symbol s is (8 - s) mod 8.
        return static_cast<std::uint8_t>((8 - tables::fixed_pn[index % 256]) & 7);
    }
    const std::size_t fixed_symbols = m_ == 1 ? 1 : 9;
    const std::size_t superframe_chips = (fixed_symbols + 9) * cps;
    const std::size_t j = index - tlc_chips;
    const std::size_t rep = j / superframe_chips;
    const std::size_t r = j % superframe_chips;

    std::uint8_t dibit;
    std::uint8_t pn;
    if (r < fixed_symbols * cps) {
        // Fixed: 9 Walsh symbols {0,0,2,1,2,1,0,2,3}, or one of di-bit 3 when M = 1 (D.5.2.1.3).
        dibit = m_ == 1 ? std::uint8_t{3} : tables::fixed_dibits[r / cps];
        pn = tables::fixed_pn[r % 256];
    } else if (r < (fixed_symbols + 4) * cps) {
        // Downcount, M-1 in the first super-frame down to 0 in the last.
        const std::size_t pos = r - fixed_symbols * cps;
        const auto count = encode_count(m_ - 1 - static_cast<int>(rep));
        dibit = (*count)[pos / cps];
        pn = tables::cnt_pn[pos];
    } else {
        const std::size_t pos = r - (fixed_symbols + 4) * cps;
        dibit = wid_[pos / cps];
        pn = tables::wid_pn[pos];
    }
    return static_cast<std::uint8_t>((tables::walsh[dibit][r & 3] + pn) & 7);
}

void Preamble::write(std::span<std::uint8_t> out) const noexcept {
    const std::size_t n = chip_count();
    for (std::size_t i = 0; i < n && i < out.size(); ++i) out[i] = chip(i);
}

}  // namespace m0lte::ms110d
