// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Internal tables of MIL-STD-188-110D Appendix D (3 kHz serial tone). Not part of the public
// API and not installed: the library and its tests include this from src/. Every value is
// ported from the C# Ms110dTables, MiniProbe and the modulator's transcoding switch, and is
// checked against the C# by tests/csharp_tables_test.cpp. The d-numbers beside each table name
// its transcription in the pdn-soundmodem repository (docs/dev/ms110d/tables).
//
// Everything here is constexpr data with no allocation, and holds no computed floating point
// values (only literals), so it is safe to include under any consumer's flags.

#include <m0lte/ms110d/mode.hpp>

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d::tables {

// Single precision, as the C# transmits: the audio is compared against files made from these.
using Complex32 = std::complex<float>;

// Chips per preamble channel symbol at 3 kHz (Table D-XIII row 3, d13).
inline constexpr int preamble_chips_per_symbol = 32;
// Symbol rate at 3 kHz (Table D-I, d01).
inline constexpr int symbol_rate = 2400;
// Audio sub-carrier at 3 kHz (Table D-I, d01).
inline constexpr int sub_carrier_hz = 1800;
// EOM marker, 32 bits, most significant bit sent first (D.5.4.3). No transcription file; the
// C# value, also visible in the EOM bits of the reference files.
inline constexpr std::uint32_t eom = 0x4B65A5B2u;

// Table D-XVIII fixedPN, d18: TLC and Fixed preamble scrambling sequence, 8PSK symbol numbers.
inline constexpr std::array<std::uint8_t, 256> fixed_pn{{
    2, 4, 0, 0, 6, 2, 1, 4, 6, 1, 0, 5, 7, 3, 4, 1, 2, 6, 1, 7, 0, 7, 3, 2, 2, 2, 3, 2, 4, 6, 3, 6,
    6, 3, 7, 5, 4, 7, 5, 6, 7, 4, 0, 2, 6, 1, 5, 3, 0, 4, 2, 4, 6, 4, 5, 2, 5, 4, 5, 3, 1, 5, 4, 5,
    6, 5, 1, 0, 7, 1, 0, 1, 0, 5, 3, 5, 2, 2, 4, 5, 4, 0, 6, 4, 1, 4, 0, 3, 3, 0, 0, 3, 3, 7, 3, 4,
    2, 7, 4, 4, 4, 0, 3, 4, 7, 6, 4, 2, 6, 2, 0, 3, 5, 3, 2, 2, 4, 5, 2, 0, 0, 3, 5, 0, 3, 2, 6, 6,
    1, 4, 2, 3, 6, 1, 3, 0, 3, 3, 2, 4, 2, 2, 6, 5, 5, 3, 6, 7, 6, 5, 6, 6, 5, 2, 5, 4, 2, 3, 3, 3,
    5, 7, 5, 5, 3, 7, 0, 4, 7, 0, 4, 1, 6, 2, 3, 5, 5, 6, 2, 6, 4, 6, 3, 4, 0, 7, 0, 0, 5, 2, 1, 5,
    4, 3, 4, 5, 7, 0, 5, 3, 7, 6, 6, 6, 4, 5, 6, 0, 2, 0, 4, 2, 3, 4, 4, 0, 7, 6, 6, 2, 0, 0, 3, 3,
    0, 5, 2, 4, 2, 2, 4, 5, 4, 6, 6, 6, 3, 2, 1, 0, 3, 2, 6, 0, 6, 2, 4, 0, 6, 4, 1, 3, 3, 5, 3, 6,
}};

// Table D-XIX cntPN, d19: Count preamble scrambling sequence.
inline constexpr std::array<std::uint8_t, 256> cnt_pn{{
    5, 5, 2, 2, 0, 2, 5, 6, 7, 1, 3, 5, 1, 5, 6, 5, 3, 7, 0, 4, 0, 3, 3, 2, 1, 3, 0, 3, 1, 6, 2, 6,
    0, 6, 4, 1, 2, 5, 6, 3, 5, 3, 7, 4, 2, 6, 7, 3, 0, 2, 0, 1, 7, 5, 0, 6, 1, 5, 0, 3, 2, 2, 5, 2,
    5, 2, 3, 4, 2, 7, 6, 1, 1, 5, 2, 1, 5, 4, 0, 3, 5, 5, 0, 3, 1, 4, 0, 5, 0, 3, 0, 6, 0, 0, 3, 1,
    6, 1, 4, 4, 7, 7, 0, 5, 7, 0, 1, 5, 1, 0, 1, 3, 1, 5, 0, 7, 1, 2, 2, 2, 7, 1, 2, 5, 0, 3, 3, 2,
    2, 0, 4, 5, 1, 3, 1, 3, 5, 3, 1, 7, 5, 2, 7, 1, 3, 1, 5, 6, 2, 4, 6, 0, 6, 1, 0, 0, 3, 6, 2, 7,
    3, 2, 4, 7, 6, 4, 1, 3, 6, 6, 0, 3, 0, 0, 7, 5, 4, 5, 1, 2, 1, 5, 0, 3, 1, 0, 4, 6, 6, 1, 0, 5,
    2, 6, 3, 2, 7, 4, 2, 4, 0, 1, 7, 0, 7, 0, 5, 1, 4, 5, 7, 2, 0, 4, 4, 3, 5, 2, 7, 7, 4, 5, 1, 4,
    4, 6, 3, 3, 0, 5, 1, 5, 5, 4, 3, 2, 0, 3, 0, 4, 7, 4, 5, 1, 5, 5, 7, 7, 6, 2, 4, 3, 5, 2, 2, 4,
}};

// Table D-XX widPN, d20: Waveform ID preamble scrambling sequence.
inline constexpr std::array<std::uint8_t, 256> wid_pn{{
    2, 3, 0, 3, 7, 3, 3, 0, 1, 4, 4, 6, 5, 5, 4, 5, 6, 2, 0, 5, 6, 6, 5, 3, 5, 5, 2, 2, 1, 2, 3, 6,
    1, 1, 4, 3, 1, 0, 5, 1, 0, 3, 3, 0, 3, 0, 4, 4, 6, 2, 5, 6, 1, 7, 2, 6, 2, 0, 0, 4, 7, 2, 3, 5,
    2, 7, 1, 6, 5, 0, 4, 1, 6, 2, 1, 5, 4, 3, 5, 0, 3, 4, 1, 3, 2, 1, 6, 1, 5, 7, 0, 4, 7, 6, 6, 0,
    4, 7, 6, 6, 6, 6, 2, 3, 5, 0, 7, 0, 3, 1, 5, 1, 2, 0, 5, 3, 2, 4, 5, 6, 6, 7, 7, 3, 5, 1, 6, 0,
    1, 4, 4, 5, 6, 0, 6, 7, 2, 4, 4, 0, 3, 7, 2, 0, 0, 1, 4, 0, 7, 1, 7, 4, 5, 4, 5, 5, 5, 3, 3, 2,
    0, 5, 1, 3, 1, 5, 3, 4, 1, 5, 4, 1, 4, 4, 2, 2, 4, 3, 0, 7, 4, 1, 5, 7, 1, 4, 7, 2, 5, 5, 6, 6,
    1, 6, 5, 6, 3, 0, 2, 5, 7, 7, 4, 4, 3, 4, 4, 6, 0, 7, 2, 2, 0, 0, 2, 1, 0, 0, 3, 6, 6, 4, 0, 2,
    4, 3, 4, 5, 2, 6, 3, 7, 7, 5, 7, 3, 0, 7, 0, 0, 7, 2, 6, 2, 2, 6, 1, 4, 3, 7, 6, 5, 0, 6, 5, 4,
}};

// Table D-XIV, d14: 4-ary Walsh sequences by di-bit (00 -> 0000, 01 -> 0404, 10 -> 0044,
// 11 -> 0440), repeated to the Table D-XIII length. Indexed by the di-bit value.
inline constexpr std::array<std::array<std::uint8_t, 4>, 4> walsh{{
    {{0, 0, 0, 0}},
    {{0, 4, 0, 4}},
    {{0, 0, 4, 4}},
    {{0, 4, 4, 0}},
}};

// Fixed-subsection di-bits for the 9-Walsh-symbol case, 3 sent last (D.5.2.1.3,
// preamble-fixed-tlc-prose.md).
inline constexpr std::array<std::uint8_t, 9> fixed_dibits{{0, 0, 2, 1, 2, 1, 0, 2, 3}};

// Table D-III, d03: BPSK transcoding. Index = bit, value = 8PSK symbol number.
inline constexpr std::array<std::uint8_t, 2> transcode_bpsk{{0, 4}};
// Table D-IV, d04: QPSK transcoding. Index = di-bit value (first fetched bit is the MSB).
inline constexpr std::array<std::uint8_t, 4> transcode_qpsk{{0, 2, 6, 4}};
// Table D-V, d05: 8PSK transcoding. Index = tribit value (MSB first), value = ring symbol.
inline constexpr std::array<std::uint8_t, 8> transcode_8psk{{1, 0, 2, 3, 6, 7, 5, 4}};

// Table D-VI, d06: 8PSK constellation, symbol s at phase s * 45 degrees, printed values.
inline constexpr std::array<Complex32, 8> psk8{{
    {1.000000f, 0.000000f},
    {0.707107f, 0.707107f},
    {0.000000f, 1.000000f},
    {-0.707107f, 0.707107f},
    {-1.000000f, 0.000000f},
    {-0.707107f, -0.707107f},
    {0.000000f, -1.000000f},
    {0.707107f, -0.707107f},
}};

// Table D-VII, d07: 16-QAM constellation, printed values.
inline constexpr std::array<Complex32, 16> qam16{{
    {0.866025f, 0.500000f},
    {0.500000f, 0.866025f},
    {1.000000f, 0.000000f},
    {0.258819f, 0.258819f},
    {-0.500000f, 0.866025f},
    {0.000000f, 1.000000f},
    {-0.866025f, 0.500000f},
    {-0.258819f, 0.258819f},
    {0.500000f, -0.866025f},
    {0.000000f, -1.000000f},
    {0.866025f, -0.500000f},
    {0.258819f, -0.258819f},
    {-0.866025f, -0.500000f},
    {-0.500000f, -0.866025f},
    {-1.000000f, 0.000000f},
    {-0.258819f, -0.258819f},
}};

// Table D-XXII(a), d22a: 13-symbol mini-probe base (Barker-13).
inline constexpr std::array<Complex32, 13> base13{{
    {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {-1, 0}, {-1, 0},
    {1, 0}, {1, 0}, {-1, 0}, {1, 0}, {-1, 0}, {1, 0},
}};

// Table D-XXIII, d23: 16-symbol mini-probe base.
inline constexpr std::array<Complex32, 16> base16{{
    {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {0, -1}, {-1, 0}, {0, 1},
    {1, 0}, {-1, 0}, {1, 0}, {-1, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1},
}};

// Table D-XXIV, d24: 19-symbol mini-probe base. Not used by the waveforms here (probe length
// 36 is WN 11 and 12); carried for completeness.
inline constexpr std::array<Complex32, 19> base19{{
    {1, 0}, {-1, 0}, {1, 0}, {1, 0}, {-1, 0}, {-1, 0}, {-1, 0},
    {-1, 0}, {1, 0}, {-1, 0}, {1, 0}, {-1, 0}, {1, 0}, {1, 0},
    {1, 0}, {1, 0}, {-1, 0}, {-1, 0}, {1, 0},
}};

// Table D-XXV, d25: 25-symbol mini-probe base. The six-decimal values are the ones the C#
// transmits, verbatim, including 0.951056 against 0.951057 and 0.309016 against 0.309017
// (the exact values are the fifth roots of unity; see issue 16). Changing any digit changes what goes on air.
inline constexpr std::array<Complex32, 25> base25{{
    {1.000000f, 0.000000f}, {1.000000f, 0.000000f}, {1.000000f, 0.000000f},
    {1.000000f, 0.000000f}, {1.000000f, 0.000000f}, {1.000000f, 0.000000f},
    {0.309017f, -0.951057f}, {-0.809017f, -0.587785f}, {-0.809017f, 0.587785f},
    {0.309017f, 0.951056f}, {1.000000f, 0.000000f}, {-0.809017f, -0.587785f},
    {0.309017f, 0.951056f}, {0.309017f, -0.951057f}, {-0.809017f, 0.587785f},
    {1.000000f, 0.000000f}, {-0.809017f, 0.587785f}, {0.309017f, -0.951057f},
    {0.309017f, 0.951057f}, {-0.809017f, -0.587785f}, {1.000000f, 0.000000f},
    {0.309017f, 0.951056f}, {-0.809017f, 0.587785f}, {-0.809017f, -0.587785f},
    {0.309016f, -0.951057f},
}};

// Table D-XXI (the 3 kHz rows), d21: a mini-probe of length k is its base sequence cyclically
// extended to k; the probe before an interleaver boundary starts `shift` symbols into the
// base. (The direction of the shift is the C#'s interpretation; issue 16.)
struct ProbeSpec {
    int k;
    std::span<const Complex32> base;
    int shift;
};

inline constexpr std::array<ProbeSpec, 4> probe_specs{{
    {24, base13, 6},
    {32, base16, 8},
    {36, base19, 9},
    {48, base25, 12},
}};

// The spec for a probe length, or nullptr if it is not one of the 3 kHz lengths carried.
constexpr const ProbeSpec* find_probe_spec(int k) noexcept {
    for (const ProbeSpec& s : probe_specs) {
        if (s.k == k) return &s;
    }
    return nullptr;
}

// Symbol i of a k-symbol probe, shifted for the interleaver boundary or not. i may run past k
// (the EOT marker extends the last probe cyclically by 32 symbols). Requires i >= 0 and a
// spec from probe_specs (a negative i would index before the base sequence).
constexpr Complex32 probe_symbol(const ProbeSpec& spec, int i, bool boundary) noexcept {
    const int offset = boundary ? spec.shift : 0;
    return spec.base[static_cast<std::size_t>((i + offset) % static_cast<int>(spec.base.size()))];
}

}  // namespace m0lte::ms110d::tables
