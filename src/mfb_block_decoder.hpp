// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The MFB-form block decoder of the C# (Ms110dMfbBlockDecoder, M0LTE.Ms110d 0.4.1): the QAM16
// block decoder that replaces the turbo loop for WN 8, and the second member of the 8PSK (WN 7)
// per-block ensemble. Per burst, a ridged wide least-squares fit over the first block's probes
// places a 26 tap detection window; per block, label-free composite-FIR anchors on the probes,
// per-tap interpolation, a linear MMSE cold rung, then soft and hard cancellation rungs with one
// gated decision-directed anchor re-fit, to an exact fixed point (or the better member of a
// two-cycle), else the caller keeps its own decode. See the C# for the measurements behind each
// constant; they are copied, not re-derived.
//
// The C# does this in System.Numerics.Complex, double precision; so does this port, in both
// builds, with the .NET operations written out (NetComplex below) so the arithmetic is the same.
// Internal; used only by DfeReceiver. No allocation after configure, no exceptions.

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/mode.hpp>

#include "fec/convolutional.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "interleaver.hpp"
#include "puncture.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

// System.Numerics.Complex arithmetic, operation for operation (the products as .NET forms them,
// Smith's division, the overflow-safe magnitude).
struct NetComplex {
    double re = 0;
    double im = 0;
};

inline NetComplex operator+(NetComplex a, NetComplex b) noexcept { return {a.re + b.re, a.im + b.im}; }
inline NetComplex operator-(NetComplex a, NetComplex b) noexcept { return {a.re - b.re, a.im - b.im}; }
inline NetComplex operator*(NetComplex a, NetComplex b) noexcept {
    return {(a.re * b.re) - (a.im * b.im), (a.im * b.re) + (a.re * b.im)};
}
inline NetComplex operator*(NetComplex a, double s) noexcept { return {a.re * s, a.im * s}; }
inline NetComplex operator*(double s, NetComplex a) noexcept { return {s * a.re, s * a.im}; }
inline NetComplex operator/(NetComplex a, double s) noexcept { return {a.re / s, a.im / s}; }
inline NetComplex operator/(NetComplex l, NetComplex r) noexcept {
    const double a = l.re, b = l.im, c = r.re, d = r.im;
    if (std::abs(d) < std::abs(c)) {
        const double doc = d / c;
        return {(a + (b * doc)) / (c + (d * doc)), (b - (a * doc)) / (c + (d * doc))};
    }
    const double cod = c / d;
    return {(b + (a * cod)) / (d + (c * cod)), (-a + (b * cod)) / (d + (c * cod))};
}
inline bool operator==(NetComplex a, NetComplex b) noexcept { return a.re == b.re && a.im == b.im; }
inline NetComplex conj(NetComplex a) noexcept { return {a.re, -a.im}; }
inline double magnitude(NetComplex v) noexcept {
    const double a = std::abs(v.re);
    const double b = std::abs(v.im);
    const double small = a < b ? a : b;
    const double large = a < b ? b : a;
    if (small == 0.0) return large;
    if (std::isinf(large) && !std::isnan(small)) return large;
    const double ratio = small / large;
    return large * std::sqrt(1.0 + (ratio * ratio));
}


#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

class DfeReceiver;

class MfbBlockDecoder {
public:
    static constexpr int scan_min = -18;
    static constexpr int scan_max = 19;
    static constexpr int scan_len = scan_max - scan_min;
    static constexpr int win_len = 26;

    MfbBlockDecoder() noexcept = default;
    MfbBlockDecoder(const MfbBlockDecoder&) = delete;
    MfbBlockDecoder& operator=(const MfbBlockDecoder&) = delete;

    // Storage for the 8PSK and QAM16 modes with interleavers up to max_interleaver.
    static std::size_t storage_bytes(InterleaverKind max_interleaver) noexcept;
    bool configure(std::span<std::uint8_t> storage, InterleaverKind max_interleaver) noexcept;

    // A new burst (the C# builds a fresh decoder per burst): the mode and its coding, and the
    // detection window to be scanned again. False if the mode is not 8PSK or QAM16 or too big.
    bool begin(const Mode& mode, const InterleaverParams& il, const fec::ConvolutionalCode& code,
               const PunctureSpec& puncture, Interleaver interleaver, fec::TailBitingViterbiDecoder& viterbi,
               fec::TailBitingSisoDecoder& siso) noexcept;

    // Decodes the receiver's current block in place: true at a fixed point (the decode is in
    // `info`), false when there is none by the cap (the caller keeps its decode).
    bool decode_block(const DfeReceiver& rx, const std::int64_t* frame_chips, std::uint8_t* info) noexcept;

    // The label-free price of a decode of the last block: the mean squared residual of its
    // reconstruction through the block's anchors.
    double price(const std::uint8_t* info, const std::int64_t* frame_chips) noexcept;
    // Complex samples a price averages over.
    std::int64_t price_rows() const noexcept {
        return 2 * (static_cast<std::int64_t>(il_.frames - 1) * (mode_.u + mode_.k) + mode_.u);
    }

private:
    NetComplex wire(int number, int u) const noexcept;
    NetComplex point(int q) const noexcept;
    bool probe_is_boundary(int p, int frames) const noexcept;
    void probe_symbols(bool boundary, NetComplex* out) const noexcept;
    void set_decisions_from(const std::uint8_t* info) noexcept;
    void set_decisions_soft() noexcept;
    void decode(const float* llrs, std::uint8_t* info) noexcept;
    bool run_schedule(bool soft_first, const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min) noexcept;
    void scan_window(const std::int64_t* frame_chips, std::int64_t hc0) noexcept;
    void refit_anchors(const std::int64_t* frame_chips, std::int64_t hc0) noexcept;
    void rebuild_anchors(bool mid_frame) noexcept;
    void sort_anchors() noexcept;
    void interpolate_h(double chip, NetComplex* h) const noexcept;
    double recon_residual(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min) noexcept;
    void project(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min, bool cancel) noexcept;
    void project_mmse_cold(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min) noexcept;
    void build_llrs() noexcept;
    void clear_normal(int len) noexcept;
    void accumulate_row(NetComplex row, int len) noexcept;
    bool solve_ridged(int len, double ridge_scale) noexcept;
    static bool solve_hermitian(NetComplex* a, int stride, NetComplex* b, int n) noexcept;
    const NetComplex* anchor_h(int sorted_index) const noexcept;

    // Storage (the longest block of the configured modes).
    NetComplex* ring_ = nullptr;
    NetComplex* recon_ = nullptr;
    NetComplex* decisions_ = nullptr;
    NetComplex* proj_ = nullptr;
    double* gram_t_ = nullptr;
    std::int64_t* data_chip_ = nullptr;
    float* llrs_ = nullptr;
    std::uint8_t* dec_ = nullptr;
    std::uint8_t* prev_dec_ = nullptr;
    std::uint8_t* prev_dec2_ = nullptr;
    std::uint8_t* dec_a_ = nullptr;
    std::uint8_t* fetched_ = nullptr;
    std::uint8_t* coded_ = nullptr;
    std::uint8_t* punctured_bits_ = nullptr;
    float* soft_punctured_ = nullptr;
    float* soft_mother_ = nullptr;
    float* soft_mother_post_ = nullptr;
    float* soft_wire_ = nullptr;
    float* mother_ = nullptr;
    // Anchors: the probe anchors, then the re-fit ones, and the sorted order over both.
    double* anchor_chip_ = nullptr;   // max_anchors
    NetComplex* anchor_h_ = nullptr;  // max_anchors * win_len
    int* order_ = nullptr;            // max_anchors
    int probe_anchors_ = 0;
    int anchors_ = 0;                 // probe and re-fit anchors stored
    int ordered_ = 0;                 // entries of order_ in use
    std::size_t max_symbols_ = 0;
    std::size_t max_ring_ = 0;
    std::size_t max_size_bits_ = 0;
    std::size_t max_input_bits_ = 0;
    int max_anchors_ = 0;

    // The burst.
    Mode mode_{};
    InterleaverParams il_{};
    const fec::ConvolutionalCode* code_ = nullptr;
    const PunctureSpec* puncture_ = nullptr;
    Interleaver interleaver_{0, 0};
    fec::TailBitingViterbiDecoder* viterbi_ = nullptr;
    fec::TailBitingSisoDecoder* siso_ = nullptr;
    bool qam_ = false;
    int bps_ = 0;
    int points_ = 0;
    std::array<int, 256> scr_{};
    double es_ = 0;
    bool window_set_ = false;
    int l_min_ = 0;
    int long_frames_ = 1;
    std::int64_t block_base_ = 0;
    std::int64_t last_hc0_ = 0;
    int last_n_ = 0;
    bool last_cycle_accepted_ = false;
    double sigma2_ = 0;
    double anchor_resid_sum_ = 0;
    std::int64_t anchor_resid_rows_ = 0;

    // Scratch.
    std::array<NetComplex, win_len * win_len> gram_{};
    std::array<NetComplex, win_len> rhs_{};
    std::array<NetComplex, win_len> phi_row_{};
    std::array<NetComplex, win_len * win_len> r_mat_{};
    std::array<NetComplex, win_len> z_vec_{};
    std::array<NetComplex, win_len> h_col_{};
    std::array<NetComplex, win_len> h_nb_{};
    std::array<NetComplex, win_len> h_shift_{};
    std::array<NetComplex, win_len> h_at_{};
    std::array<NetComplex, scan_len * scan_len> scan_gram_{};
    std::array<NetComplex, scan_len> scan_rhs_{};
    std::array<NetComplex, scan_len> scan_phi_{};
    std::array<double, 16> metric_{};
    std::array<double, 4> p0_{};
    std::array<NetComplex, 48> probe_{};
};

}  // inline namespace

}  // namespace m0lte::ms110d
