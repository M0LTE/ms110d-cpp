#pragma once

#include <optional>
#include <span>

namespace m0lte::ms110d {

// The 3 kHz serial-tone waveforms of MIL-STD-188-110D Appendix D: which waveform
// numbers (WN) exist, how each modulates, its frame geometry and code rate, and the
// interleaver sizes. Counterpart of the C# Ms110dMode and Ms110dInterleaverParams. The
// numbers here are integers, so none of this depends on the floating point precision.

// Data-symbol modulation per waveform number (Table D-II).
enum class Modulation {
    Walsh,  // WN 0: 32-chip 4-ary orthogonal Walsh channel symbols, no mini-probes
    Bpsk,   // WN 1 to 5
    Qpsk,   // WN 6 and 13
    Psk8,   // WN 7
    Qam16,  // WN 8
};

// Interleaver length option, signalled by WID bits d5d4 (Table D-XVI).
enum class InterleaverKind {
    UltraShort,  // d5d4 = 00
    Short,       // 01
    Medium,      // 10
    Long,        // 11
};

// A convolutional code rate as printed in Table D-XLIX, for example 3/4.
struct CodeRate {
    int numerator;
    int denominator;
    bool operator==(const CodeRate&) const = default;
};

// One 3 kHz waveform-number row (Tables D-II, D-XI, D-XII and D-XLIX).
struct Mode {
    int wn;               // waveform number, 0 to 8 or 13
    Modulation modulation;
    int bits_per_symbol;  // coded bits per data channel symbol (Walsh: per 32-chip symbol)
    int u;                // unknown (data) symbols per frame; 0 for WN 0, which is continuous
    int k;                // known (mini-probe) symbols per frame; 0 for WN 0
    CodeRate code_rate;
    int bps;              // user data rate, bits per second
};

// The interleaver geometry for one (waveform, length) pair (Tables D-XXXVII and D-LI).
struct InterleaverParams {
    int frames;      // data frames per interleaver block (WN 0: Walsh channel symbols)
    int size_bits;   // interleaver size in bits
    int input_bits;  // info bits per interleaver block (the tail-biting block length)
    int increment;   // interleaver load increment
};

// Every 3 kHz mode this library implements: WN 0 to 8, then 13. WN 9 to 12 are not
// implemented.
std::span<const Mode> modes_3k() noexcept;

// The 3 kHz mode for a waveform number, or nothing if it is not implemented.
std::optional<Mode> mode_3k(int wn) noexcept;

// Whether Table D-XXXVII defines this (waveform, interleaver) pair: WN 0 has no UltraShort
// row, and only the waveforms in modes_3k() exist. A noise-corrupted WID can pass its
// checksum and name an undefined pair, so a receiver checks this before using it.
bool has_interleaver_3k(int wn, InterleaverKind kind) noexcept;

// The 3 kHz interleaver parameters, or nothing where has_interleaver_3k is false.
std::optional<InterleaverParams> interleaver_3k(int wn, InterleaverKind kind) noexcept;

}  // namespace m0lte::ms110d
