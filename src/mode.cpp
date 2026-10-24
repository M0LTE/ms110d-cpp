#include <m0lte/ms110d/mode.hpp>

#include <array>

namespace m0lte::ms110d {

namespace {

// Table D-II row 3 (rates), D-XI and D-XII row 3 (U and K), D-XLIX row 3 (code rates).
constexpr std::array<Mode, 10> k_modes{{
    {0, Modulation::Walsh, 2, 0, 0, {1, 2}, 75},
    {1, Modulation::Bpsk, 1, 48, 48, {1, 8}, 150},
    {2, Modulation::Bpsk, 1, 48, 48, {1, 4}, 300},
    {3, Modulation::Bpsk, 1, 96, 32, {1, 3}, 600},
    {4, Modulation::Bpsk, 1, 96, 32, {2, 3}, 1200},
    {5, Modulation::Bpsk, 1, 256, 32, {3, 4}, 1600},
    {6, Modulation::Qpsk, 2, 256, 32, {3, 4}, 3200},
    {7, Modulation::Psk8, 3, 256, 32, {3, 4}, 4800},
    {8, Modulation::Qam16, 4, 256, 32, {3, 4}, 6400},
    {13, Modulation::Qpsk, 2, 256, 32, {9, 16}, 2400},
}};

struct InterleaverRow {
    int wn;
    // Indexed by InterleaverKind. A row of zeros means the pair is not defined.
    std::array<InterleaverParams, 4> by_kind;
};

// Tables D-XXXVII (frames, size, input bits) and D-LI (increments), 3 kHz.
constexpr std::array<InterleaverRow, 10> k_interleavers{{
    {0, {{{0, 0, 0, 0}, {40, 80, 40, 11}, {144, 288, 144, 37}, {576, 1152, 576, 145}}}},
    {1, {{{4, 192, 24, 25}, {16, 768, 96, 97}, {64, 3072, 384, 385}, {256, 12288, 1536, 1543}}}},
    {2, {{{4, 192, 48, 25}, {16, 768, 192, 97}, {64, 3072, 768, 385}, {256, 12288, 3072, 1543}}}},
    {3, {{{2, 192, 64, 25}, {8, 768, 256, 97}, {32, 3072, 1024, 385}, {128, 12288, 4096, 1549}}}},
    {4, {{{2, 192, 128, 25}, {8, 768, 512, 97}, {32, 3072, 2048, 385}, {128, 12288, 8192, 1549}}}},
    {5, {{{1, 256, 192, 33}, {4, 1024, 768, 129}, {16, 4096, 3072, 513}, {64, 16384, 12288, 2081}}}},
    {6, {{{1, 512, 384, 65}, {4, 2048, 1536, 257}, {16, 8192, 6144, 1025}, {64, 32768, 24576, 4161}}}},
    {7, {{{1, 768, 576, 97}, {4, 3072, 2304, 385}, {16, 12288, 9216, 1537}, {64, 49152, 36864, 6241}}}},
    {8, {{{1, 1024, 768, 129}, {4, 4096, 3072, 513}, {16, 16384, 12288, 2049}, {64, 65536, 49152, 8321}}}},
    {13, {{{1, 512, 288, 65}, {4, 2048, 1152, 257}, {16, 8192, 4608, 1025}, {64, 32768, 18432, 4161}}}},
}};

}  // namespace

std::span<const Mode> modes_3k() noexcept {
    return k_modes;
}

std::optional<Mode> mode_3k(int wn) noexcept {
    for (const Mode& m : k_modes) {
        if (m.wn == wn) return m;
    }
    return std::nullopt;
}

std::optional<InterleaverParams> interleaver_3k(int wn, InterleaverKind kind) noexcept {
    const int i = static_cast<int>(kind);
    if (i < 0 || i > 3) return std::nullopt;
    for (const InterleaverRow& row : k_interleavers) {
        if (row.wn != wn) continue;
        const InterleaverParams& p = row.by_kind[static_cast<std::size_t>(i)];
        if (p.size_bits == 0) return std::nullopt;
        return p;
    }
    return std::nullopt;
}

bool has_interleaver_3k(int wn, InterleaverKind kind) noexcept {
    return interleaver_3k(wn, kind).has_value();
}

}  // namespace m0lte::ms110d
