// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mfb_block_decoder.hpp"

#include "dfe_receiver.hpp"
#include "probe_pattern.hpp"
#include "scrambler.hpp"
#include "tables.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

constexpr int win_len = MfbBlockDecoder::win_len;
constexpr int scan_min = MfbBlockDecoder::scan_min;
constexpr int scan_max = MfbBlockDecoder::scan_max;

// Schedule caps and the re-fit gate (the C# constants; see the C# for their measurements).
constexpr int soft_cap = 30;
constexpr int soft_min_rungs = 10;
constexpr int soft_plateau_rungs = 3;
constexpr double soft_plateau_tolerance = 1e-3;
constexpr int total_cap = 72;
constexpr double refit_churn_gate = 0.05;

inline NetComplex widen(tables::Complex32 v) noexcept {
    return {static_cast<double>(v.real()), static_cast<double>(v.imag())};
}

struct Need {
    std::size_t symbols = 0, ring = 0, size_bits = 0, input_bits = 0;
    int anchors = 0;
};

Need need_for(InterleaverKind max_kind) noexcept {
    Need n;
    for (const Mode& m : modes_3k()) {
        if (m.modulation != Modulation::Psk8 && m.modulation != Modulation::Qam16) continue;
        for (int kind = 0; kind <= static_cast<int>(max_kind); ++kind) {
            const auto il = interleaver_3k(m.wn, static_cast<InterleaverKind>(kind));
            if (!il) continue;
            const std::size_t frames = static_cast<std::size_t>(il->frames);
            n.symbols = std::max(n.symbols, frames * static_cast<std::size_t>(m.u));
            const std::size_t span = (2 * (((frames - 1) * static_cast<std::size_t>(m.u + m.k)) +
                                           static_cast<std::size_t>(m.u + (2 * m.k)))) +
                                     static_cast<std::size_t>(2 * (scan_max - scan_min));
            n.ring = std::max(n.ring, span);
            n.size_bits = std::max(n.size_bits, static_cast<std::size_t>(il->size_bits));
            n.input_bits = std::max(n.input_bits, static_cast<std::size_t>(il->input_bits));
            n.anchors = std::max(n.anchors, (3 * il->frames) + 1);
        }
    }
    return n;
}

struct Carver {
    std::uint8_t* base;
    std::size_t at = 0;
    template <class T>
    T* take(std::size_t count) noexcept {
        at = (at + alignof(T) - 1) / alignof(T) * alignof(T);
        T* p = base == nullptr ? nullptr : reinterpret_cast<T*>(base + at);
        at += count * sizeof(T);
        return p;
    }
};

template <class F>
void carve(Carver& c, const Need& n, F&& assign) noexcept {
    assign(c.take<NetComplex>(n.ring), c.take<NetComplex>(n.ring), c.take<NetComplex>(n.symbols),
           c.take<NetComplex>(n.symbols), c.take<double>(n.symbols), c.take<std::int64_t>(n.symbols),
           c.take<float>(n.size_bits), c.take<std::uint8_t>(n.input_bits), c.take<std::uint8_t>(n.input_bits),
           c.take<std::uint8_t>(n.input_bits), c.take<std::uint8_t>(n.input_bits), c.take<std::uint8_t>(n.size_bits),
           c.take<std::uint8_t>(2 * n.input_bits), c.take<std::uint8_t>(n.size_bits), c.take<float>(n.size_bits),
           c.take<float>(2 * n.input_bits), c.take<float>(2 * n.input_bits), c.take<float>(n.size_bits),
           c.take<float>(2 * n.input_bits), c.take<double>(static_cast<std::size_t>(n.anchors)),
           c.take<NetComplex>(static_cast<std::size_t>(n.anchors) * win_len),
           c.take<int>(static_cast<std::size_t>(n.anchors)));
}

}  // namespace

std::size_t MfbBlockDecoder::storage_bytes(InterleaverKind max_interleaver) noexcept {
    const Need n = need_for(max_interleaver);
    if (n.symbols == 0) return 0;
    Carver c{nullptr};
    carve(c, n, [](auto...) {});
    return c.at + alignof(double);
}

bool MfbBlockDecoder::configure(std::span<std::uint8_t> storage, InterleaverKind max_interleaver) noexcept {
    ring_ = nullptr;
    const std::size_t need = storage_bytes(max_interleaver);
    if (need == 0 || storage.size() < need) return false;
    void* where = storage.data();
    std::size_t space = storage.size();
    if (std::align(alignof(double), need - alignof(double), where, space) == nullptr) return false;
    const Need n = need_for(max_interleaver);
    Carver c{static_cast<std::uint8_t*>(where)};
    carve(c, n,
          [this](NetComplex* ring, NetComplex* recon, NetComplex* decisions, NetComplex* proj, double* gram_t,
                 std::int64_t* data_chip, float* llrs, std::uint8_t* dec, std::uint8_t* prev_dec,
                 std::uint8_t* prev_dec2, std::uint8_t* dec_a, std::uint8_t* fetched, std::uint8_t* coded,
                 std::uint8_t* punctured_bits, float* soft_punctured, float* soft_mother, float* soft_mother_post,
                 float* soft_wire, float* mother, double* anchor_chip, NetComplex* anchor_h, int* order) {
              ring_ = ring;
              recon_ = recon;
              decisions_ = decisions;
              proj_ = proj;
              gram_t_ = gram_t;
              data_chip_ = data_chip;
              llrs_ = llrs;
              dec_ = dec;
              prev_dec_ = prev_dec;
              prev_dec2_ = prev_dec2;
              dec_a_ = dec_a;
              fetched_ = fetched;
              coded_ = coded;
              punctured_bits_ = punctured_bits;
              soft_punctured_ = soft_punctured;
              soft_mother_ = soft_mother;
              soft_mother_post_ = soft_mother_post;
              soft_wire_ = soft_wire;
              mother_ = mother;
              anchor_chip_ = anchor_chip;
              anchor_h_ = anchor_h;
              order_ = order;
          });
    max_symbols_ = n.symbols;
    max_ring_ = n.ring;
    max_size_bits_ = n.size_bits;
    max_input_bits_ = n.input_bits;
    max_anchors_ = n.anchors;
    return true;
}

bool MfbBlockDecoder::begin(const Mode& mode, const InterleaverParams& il, const fec::ConvolutionalCode& code,
                            const PunctureSpec& puncture, Interleaver interleaver,
                            fec::TailBitingViterbiDecoder& viterbi, fec::TailBitingSisoDecoder& siso) noexcept {
    if (ring_ == nullptr) return false;
    if (mode.modulation != Modulation::Psk8 && mode.modulation != Modulation::Qam16) return false;
    if (static_cast<std::size_t>(il.frames) * static_cast<std::size_t>(mode.u) > max_symbols_ ||
        static_cast<std::size_t>(il.size_bits) > max_size_bits_ ||
        static_cast<std::size_t>(il.input_bits) > max_input_bits_ || mode.u > 256 || mode.k > 48) {
        return false;
    }
    mode_ = mode;
    il_ = il;
    code_ = &code;
    puncture_ = &puncture;
    interleaver_ = interleaver;
    viterbi_ = &viterbi;
    siso_ = &siso;
    qam_ = mode.modulation == Modulation::Qam16;
    bps_ = mode.bits_per_symbol;
    points_ = 1 << bps_;
    Scrambler scrambler;
    scrambler.reset();
    for (int i = 0; i < mode.u; ++i) scr_[static_cast<std::size_t>(i)] = qam_ ? scrambler.next_qam(0, 4) : scrambler.next_psk(0);
    double es = 0;
    for (int q = 0; q < points_; ++q) {
        const NetComplex pt = point(q);
        es += (pt.re * pt.re) + (pt.im * pt.im);
    }
    es_ = es / points_;
    window_set_ = false;
    l_min_ = 0;
    return true;
}

NetComplex MfbBlockDecoder::wire(int number, int u) const noexcept {
    const int s = scr_[static_cast<std::size_t>(u)];
    if (qam_) return widen(tables::qam16[static_cast<std::size_t>(number ^ s)]);
    return widen(tables::psk8[static_cast<std::size_t>((tables::transcode_8psk[static_cast<std::size_t>(number)] + s) & 7)]);
}

NetComplex MfbBlockDecoder::point(int q) const noexcept {
    return qam_ ? widen(tables::qam16[static_cast<std::size_t>(q)]) : widen(tables::psk8[static_cast<std::size_t>(q)]);
}

// The probe at anchor p of a block: the one before frame p, or for p == frames the one after the
// last frame; shifted as the burst's probe pattern says.
bool MfbBlockDecoder::probe_is_boundary(int p, int frames) const noexcept {
    const std::int64_t after = block_base_ + (p < frames ? p - 1 : frames - 1);
    return pattern_->boundary(after);
}

void MfbBlockDecoder::probe_symbols(bool boundary, NetComplex* out) const noexcept {
    const tables::ProbeSpec* spec = tables::find_probe_spec(mode_.k);
    for (int i = 0; i < mode_.k; ++i) out[i] = widen(tables::probe_symbol(*spec, i, boundary));
}

void MfbBlockDecoder::decode(const float* llrs, std::uint8_t* info) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    interleaver_.deinterleave(llrs, soft_punctured_);
    depuncture_soft(*puncture_, {soft_punctured_, size}, {mother_, 2 * input});
    viterbi_->decode({mother_, 2 * input}, {info, input});
}

void MfbBlockDecoder::set_decisions_from(const std::uint8_t* info) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    fec::encode_tail_biting(*code_, {info, input}, {coded_, 2 * input});
    apply_puncture(*puncture_, {coded_, 2 * input}, {punctured_bits_, size});
    interleaver_.interleave(punctured_bits_, fetched_);
    const int u = mode_.u;
    const std::size_t symbols = static_cast<std::size_t>(il_.frames) * static_cast<std::size_t>(u);
    std::size_t bit = 0;
    for (std::size_t d = 0; d < symbols; ++d) {
        int number = 0;
        for (int i = 0; i < bps_; ++i) number = (number << 1) | fetched_[bit++];
        decisions_[d] = wire(number, static_cast<int>(d % static_cast<std::size_t>(u)));
    }
}

bool MfbBlockDecoder::decode_block(const DfeReceiver& rx, const std::int64_t* frame_chips, std::uint8_t* info) noexcept {
    const int u = mode_.u;
    const int k = mode_.k;
    const int frames = il_.frames;
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);

    // The block's span through the receiver's carrier- and timing-corrected read.
    const std::int64_t hc0 = (2 * (frame_chips[0] - k)) + (2 * scan_min);
    const std::int64_t hc_end = (2 * (frame_chips[frames - 1] + u + k)) + (2 * scan_max);
    const int n = static_cast<int>(hc_end - hc0);
    last_hc0_ = hc0;
    last_n_ = n;
    pattern_ = &rx.pattern_;
    block_base_ = static_cast<std::int64_t>(rx.block_index_) * frames;
    for (std::int64_t hc = hc0; hc < hc_end; ++hc) {
        const dsp::ComplexF v = rx.read_t2(static_cast<double>(hc));
        ring_[hc - hc0] = {static_cast<double>(v.re), static_cast<double>(v.im)};
    }
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < u; ++i) data_chip_[(f * u) + i] = frame_chips[f] + i;
    }

    if (!window_set_) {
        scan_window(frame_chips, hc0);
        window_set_ = true;
    }
    const int l_min = l_min_;
    const int l_max = l_min + win_len;

    // Probe anchors: each probe's least squares over its ISI-clean interior.
    probe_anchors_ = 0;
    anchor_resid_sum_ = 0;
    anchor_resid_rows_ = 0;
    for (int p = 0; p <= frames; ++p) {
        const bool preceding = p < frames;
        const std::int64_t ps = preceding ? frame_chips[p] - k : frame_chips[frames - 1] + u;
        probe_symbols(probe_is_boundary(p, frames), probe_.data());
        clear_normal(win_len);
        const std::int64_t row_lo = (2 * ps) + l_max;
        const std::int64_t row_hi = (2 * (ps + k)) + l_min;
        for (std::int64_t hc = row_lo; hc < row_hi; ++hc) {
            for (int i = 0; i < win_len; ++i) {
                const std::int64_t src = hc - (l_min + i);
                if ((src & 1) != 0) {
                    phi_row_[static_cast<std::size_t>(i)] = {};
                    continue;
                }
                phi_row_[static_cast<std::size_t>(i)] = probe_[static_cast<std::size_t>((src / 2) - ps)];
            }
            accumulate_row(ring_[hc - hc0], win_len);
        }
        if (solve_ridged(win_len, 1e-3)) {
            anchor_chip_[probe_anchors_] = static_cast<double>(ps) + (k / 2.0);
            std::copy(rhs_.begin(), rhs_.end(), anchor_h_ + (static_cast<std::size_t>(probe_anchors_) * win_len));
            ++probe_anchors_;
            // The anchor fit's residual on its own rows: the MMSE cold rung's noise estimate.
            for (std::int64_t hc = row_lo; hc < row_hi; ++hc) {
                NetComplex model{};
                for (int i = 0; i < win_len; ++i) {
                    const std::int64_t src = hc - (l_min + i);
                    if ((src & 1) != 0) continue;
                    model = model + (rhs_[static_cast<std::size_t>(i)] * probe_[static_cast<std::size_t>((src / 2) - ps)]);
                }
                const NetComplex d = ring_[hc - hc0] - model;
                anchor_resid_sum_ += (d.re * d.re) + (d.im * d.im);
                ++anchor_resid_rows_;
            }
        }
    }

    rebuild_anchors(false);

    // The soft-first schedule, then hard-first where it cannot terminate, and the dual check of
    // a cycle-accepted decode.
    bool converged = run_schedule(true, frame_chips, hc0, n, l_min);
    if (!converged) {
        rebuild_anchors(false);
        converged = run_schedule(false, frame_chips, hc0, n, l_min);
    } else if (last_cycle_accepted_) {
        std::memcpy(dec_a_, dec_, input);
        rebuild_anchors(false);
        const bool converged_b = run_schedule(false, frame_chips, hc0, n, l_min);
        if (!converged_b) {
            std::memcpy(dec_, dec_a_, input);
        } else if (last_cycle_accepted_) {
            rebuild_anchors(false);
            set_decisions_from(dec_);
            const double price_b = recon_residual(frame_chips, hc0, n, l_min);
            set_decisions_from(dec_a_);
            const double price_a = recon_residual(frame_chips, hc0, n, l_min);
            if (price_b >= price_a) std::memcpy(dec_, dec_a_, input);
        }
    }

    if (converged) std::memcpy(info, dec_, input);
    return converged;
}

bool MfbBlockDecoder::run_schedule(bool soft_first, const std::int64_t* frame_chips, std::int64_t hc0, int n,
                                   int l_min) noexcept {
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    bool have_decisions = false;
    bool converged = false;
    bool cycle_accepted = false;
    bool refit_done = false;
    int last_churn = std::numeric_limits<int>::max();
    int hard_start = soft_first ? std::numeric_limits<int>::max() : 0;
    double best_soft_sigma = std::numeric_limits<double>::max();
    int plateau_rungs = 0;
    bool plateau = false;
    const int gate = static_cast<int>(refit_churn_gate * il_.input_bits);
    for (int rung = 0; rung <= total_cap; ++rung) {
        const bool soft_phase = soft_first && rung < soft_cap && !refit_done && !plateau;
        if (!soft_phase && rung < hard_start) hard_start = rung;

        if (!soft_phase && !refit_done && (rung >= soft_cap || plateau)) {
            refit_done = true;  // handover: re-fit first if the gate admits it
            if (have_decisions && last_churn <= gate) {
                refit_anchors(frame_chips, hc0);
                rebuild_anchors(true);
            }
        }

        project(frame_chips, hc0, n, l_min, have_decisions);
        if (soft_phase && have_decisions) {
            if (sigma2_ < best_soft_sigma * (1 - soft_plateau_tolerance)) {
                best_soft_sigma = sigma2_;
                plateau_rungs = 0;
            } else if (rung >= soft_min_rungs && ++plateau_rungs >= soft_plateau_rungs) {
                plateau = true;
            }
        }

        build_llrs();
        std::memcpy(prev_dec2_, prev_dec_, input);
        std::memcpy(prev_dec_, dec_, input);
        decode(llrs_, dec_);
        int churn = 0;
        if (rung > 0) {
            for (std::size_t i = 0; i < input; ++i) churn += dec_[i] != prev_dec_[i] ? 1 : 0;
        } else {
            churn = static_cast<int>(input);
        }
        last_churn = churn;
        if (rung > 0 && churn == 0) {
            if (soft_phase && rung < soft_min_rungs) {
                set_decisions_soft();
                have_decisions = true;
                continue;
            }
            if (soft_phase) {
                // A stable soft decode hands over: re-fit and confirm it with the hard tail.
                refit_done = true;
                if (last_churn <= gate) {
                    refit_anchors(frame_chips, hc0);
                    rebuild_anchors(true);
                }
                set_decisions_from(dec_);
                have_decisions = true;
                continue;
            }
            converged = true;
            break;
        }

        // An exact two-cycle in the hard tail: keep the member whose reconstruction explains the
        // ring better.
        const bool members_hard = rung >= hard_start + 2;
        if (!soft_phase && rung > 1 && churn > 0 && members_hard && std::memcmp(dec_, prev_dec2_, input) == 0) {
            const double sigma_b = sigma2_;
            set_decisions_from(dec_);
            const double sigma_a = recon_residual(frame_chips, hc0, n, l_min);
            if (sigma_b < sigma_a) std::memcpy(dec_, prev_dec_, input);
            converged = true;
            cycle_accepted = true;
            break;
        }

        if (soft_phase) {
            set_decisions_soft();
        } else {
            set_decisions_from(dec_);
        }
        have_decisions = true;
    }
    last_cycle_accepted_ = cycle_accepted;
    return converged;
}

// ------------------------------------------------------------------ estimation

void MfbBlockDecoder::scan_window(const std::int64_t* frame_chips, std::int64_t hc0) noexcept {
    const int k = mode_.k;
    std::array<double, scan_len> profile{};
    const int scan_probes = std::min(8, il_.frames);
    for (int p = 0; p < scan_probes; ++p) {
        const std::int64_t ps = frame_chips[p] - k;
        probe_symbols(probe_is_boundary(p, il_.frames), probe_.data());
        std::fill(scan_rhs_.begin(), scan_rhs_.end(), NetComplex{});
        std::fill(scan_gram_.begin(), scan_gram_.end(), NetComplex{});
        for (std::int64_t hc = (2 * ps) + scan_max; hc < (2 * (ps + k)) + scan_min; ++hc) {
            for (int i = 0; i < scan_len; ++i) {
                const std::int64_t src = hc - (scan_min + i);
                if ((src & 1) != 0) {
                    scan_phi_[static_cast<std::size_t>(i)] = {};
                    continue;
                }
                scan_phi_[static_cast<std::size_t>(i)] = probe_[static_cast<std::size_t>((src / 2) - ps)];
            }
            const NetComplex row = ring_[hc - hc0];
            for (int i = 0; i < scan_len; ++i) {
                const NetComplex pi = conj(scan_phi_[static_cast<std::size_t>(i)]);
                scan_rhs_[static_cast<std::size_t>(i)] = scan_rhs_[static_cast<std::size_t>(i)] + (pi * row);
                for (int j = 0; j < scan_len; ++j) {
                    NetComplex& g = scan_gram_[static_cast<std::size_t>((i * scan_len) + j)];
                    g = g + (pi * scan_phi_[static_cast<std::size_t>(j)]);
                }
            }
        }
        double trace = 0;
        for (int i = 0; i < scan_len; ++i) trace += scan_gram_[static_cast<std::size_t>((i * scan_len) + i)].re;
        const double ridge = std::max(1e-9, 3e-2 * trace / scan_len);
        for (int i = 0; i < scan_len; ++i) scan_gram_[static_cast<std::size_t>((i * scan_len) + i)].re += ridge;
        if (!solve_hermitian(scan_gram_.data(), scan_len, scan_rhs_.data(), scan_len)) continue;
        for (int i = 0; i < scan_len; ++i) {
            const NetComplex r = scan_rhs_[static_cast<std::size_t>(i)];
            profile[static_cast<std::size_t>(i)] += (r.re * r.re) + (r.im * r.im);
        }
    }

    double peak = 0;
    for (int i = 0; i < scan_len; ++i) peak = std::max(peak, profile[static_cast<std::size_t>(i)]);
    int pk_lo = 0;
    int pk_hi = scan_len - 1;
    for (int i = 0; i < scan_len; ++i) {
        if (profile[static_cast<std::size_t>(i)] >= 0.1 * peak) {
            pk_lo = i;
            break;
        }
    }
    for (int i = scan_len - 1; i >= 0; --i) {
        if (profile[static_cast<std::size_t>(i)] >= 0.1 * peak) {
            pk_hi = i;
            break;
        }
    }
    l_min_ = scan_min + pk_lo - 7;
    if (scan_min + pk_hi + 8 > l_min_ + win_len) l_min_ = (scan_min + pk_hi + 8) - win_len;
}

void MfbBlockDecoder::refit_anchors(const std::int64_t* frame_chips, std::int64_t hc0) noexcept {
    // Two mid-frame decision anchors per frame, rows drawn wholly from decided data chips.
    const int u = mode_.u;
    const int l_min = l_min_;
    const int l_max = l_min + win_len;
    anchors_ = probe_anchors_;
    ordered_ = 0;
    for (int i = 0; i < probe_anchors_; ++i) order_[ordered_++] = i;
    for (int f = 0; f < il_.frames; ++f) {
        for (int half = 0; half < 2; ++half) {
            const std::int64_t start = frame_chips[f] + 16 + (half * 128);
            const std::int64_t end = start + 96;
            clear_normal(win_len);
            for (std::int64_t hc = (2 * start) + l_max; hc < (2 * end) + l_min; ++hc) {
                for (int i = 0; i < win_len; ++i) {
                    const std::int64_t src = hc - (l_min + i);
                    if ((src & 1) != 0) {
                        phi_row_[static_cast<std::size_t>(i)] = {};
                        continue;
                    }
                    phi_row_[static_cast<std::size_t>(i)] =
                        decisions_[(static_cast<std::int64_t>(f) * u) + ((src / 2) - frame_chips[f])];
                }
                accumulate_row(ring_[hc - hc0], win_len);
            }
            if (solve_ridged(win_len, 1e-3) && anchors_ < max_anchors_) {
                anchor_chip_[anchors_] = static_cast<double>(start + end) / 2.0;
                std::copy(rhs_.begin(), rhs_.end(), anchor_h_ + (static_cast<std::size_t>(anchors_) * win_len));
                order_[ordered_++] = anchors_;
                ++anchors_;
            }
        }
    }
    sort_anchors();
}

void MfbBlockDecoder::rebuild_anchors(bool mid_frame) noexcept {
    if (!mid_frame) {
        anchors_ = probe_anchors_;
        ordered_ = 0;
        for (int i = 0; i < probe_anchors_; ++i) order_[ordered_++] = i;
    }
    sort_anchors();
}

// Insertion sort by anchor time, stable, as the C#.
void MfbBlockDecoder::sort_anchors() noexcept {
    for (int i = 1; i < ordered_; ++i) {
        const int key = order_[i];
        const double t = anchor_chip_[key];
        int j = i - 1;
        while (j >= 0 && anchor_chip_[order_[j]] > t) {
            order_[j + 1] = order_[j];
            --j;
        }
        order_[j + 1] = key;
    }
}

const NetComplex* MfbBlockDecoder::anchor_h(int sorted_index) const noexcept {
    return anchor_h_ + (static_cast<std::size_t>(order_[sorted_index]) * win_len);
}

void MfbBlockDecoder::interpolate_h(double chip, NetComplex* h) const noexcept {
    if (ordered_ == 0) {
        // No anchor solved (the C# would index an empty list and throw): no response.
        std::fill(h, h + win_len, NetComplex{});
        return;
    }
    int hi = 0;
    while (hi < ordered_ && anchor_chip_[order_[hi]] < chip) ++hi;
    if (hi >= ordered_) {
        std::copy(anchor_h(ordered_ - 1), anchor_h(ordered_ - 1) + win_len, h);
        return;
    }
    if (hi == 0) {
        std::copy(anchor_h(0), anchor_h(0) + win_len, h);
        return;
    }
    const double lo_chip = anchor_chip_[order_[hi - 1]];
    const double frac = (chip - lo_chip) / (anchor_chip_[order_[hi]] - lo_chip);
    const NetComplex* a = anchor_h(hi - 1);
    const NetComplex* b = anchor_h(hi);
    for (int i = 0; i < win_len; ++i) h[i] = (a[i] * (1 - frac)) + (b[i] * frac);
}

// ------------------------------------------------------------------ detection

// Rebuilds the block from the current decisions (probes always known) and returns the mean
// squared residual per complex sample over the data span.
double MfbBlockDecoder::recon_residual(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min) noexcept {
    const int u = mode_.u;
    const int k = mode_.k;
    const int frames = il_.frames;
    std::fill(recon_, recon_ + n, NetComplex{});
    auto add_chip = [&](std::int64_t c, NetComplex x) {
        interpolate_h(static_cast<double>(c), h_at_.data());
        for (int i = 0; i < win_len; ++i) {
            const std::int64_t hc = (2 * c) + l_min + i;
            if (hc >= hc0 && hc < hc0 + n) recon_[hc - hc0] = recon_[hc - hc0] + (h_at_[static_cast<std::size_t>(i)] * x);
        }
    };
    for (int p = 0; p <= frames; ++p) {
        const bool preceding = p < frames;
        const std::int64_t ps = preceding ? frame_chips[p] - k : frame_chips[frames - 1] + u;
        probe_symbols(probe_is_boundary(p, frames), probe_.data());
        for (int c = 0; c < k; ++c) add_chip(ps + c, probe_[static_cast<std::size_t>(c)]);
    }
    const std::size_t symbols = static_cast<std::size_t>(frames) * static_cast<std::size_t>(u);
    for (std::size_t d = 0; d < symbols; ++d) add_chip(data_chip_[d], decisions_[d]);

    double resid = 0;
    std::int64_t rows = 0;
    for (std::int64_t hc = 2 * frame_chips[0]; hc < 2 * (frame_chips[frames - 1] + u); ++hc) {
        const NetComplex diff = ring_[hc - hc0] - recon_[hc - hc0];
        resid += (diff.re * diff.re) + (diff.im * diff.im);
        ++rows;
    }
    return std::max(resid / static_cast<double>(rows), 1e-12);
}

void MfbBlockDecoder::project(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min, bool cancel) noexcept {
    if (!cancel) {
        project_mmse_cold(frame_chips, hc0, n, l_min);
        return;
    }
    sigma2_ = recon_residual(frame_chips, hc0, n, l_min);
    const std::size_t symbols = static_cast<std::size_t>(il_.frames) * static_cast<std::size_t>(mode_.u);
    for (std::size_t d = 0; d < symbols; ++d) {
        const std::int64_t c = data_chip_[d];
        interpolate_h(static_cast<double>(c), h_at_.data());
        double g2 = 0;
        NetComplex acc{};
        for (int i = 0; i < win_len; ++i) {
            const std::int64_t hc = (2 * c) + l_min + i;
            if (hc < hc0 || hc >= hc0 + n) continue;
            const NetComplex h = h_at_[static_cast<std::size_t>(i)];
            const NetComplex row = (ring_[hc - hc0] - recon_[hc - hc0]) + (h * decisions_[d]);
            acc = acc + (conj(h) * row);
            g2 += (h.re * h.re) + (h.im * h.im);
        }
        proj_[d] = acc / std::max(g2, 1e-12);
        gram_t_[d] = g2;
    }
}

// The MMSE cold rung: per-symbol linear MMSE over the response window, known probes subtracted,
// neighbouring data chips as unknowns of power Es, the anchor-fit residual as the noise.
void MfbBlockDecoder::project_mmse_cold(const std::int64_t* frame_chips, std::int64_t hc0, int n, int l_min) noexcept {
    const int u = mode_.u;
    const int frames = il_.frames;
    const std::size_t symbols = static_cast<std::size_t>(frames) * static_cast<std::size_t>(u);
    std::fill(decisions_, decisions_ + symbols, NetComplex{});
    recon_residual(frame_chips, hc0, n, l_min);  // recon_ now holds the probes alone
    const NetComplex* probes_only = recon_;
    const double sigma_noise = std::max(
        anchor_resid_rows_ > 0 ? anchor_resid_sum_ / static_cast<double>(anchor_resid_rows_) : 1e-9, 1e-12);
    const int half = (win_len - 1) / 2;
    for (std::size_t d = 0; d < symbols; ++d) {
        const std::int64_t c = data_chip_[d];
        const int f0 = static_cast<int>(d / static_cast<std::size_t>(u));
        interpolate_h(static_cast<double>(c), h_col_.data());
        for (int i = 0; i < win_len; ++i) {
            for (int j = 0; j < win_len; ++j) r_mat_[static_cast<std::size_t>((i * win_len) + j)] = {};
            r_mat_[static_cast<std::size_t>((i * win_len) + i)] = {sigma_noise, 0};
        }
        for (std::int64_t cn = c - half; cn <= c + half; ++cn) {
            bool is_data = false;
            for (int f = std::max(0, f0 - 1); f <= std::min(frames - 1, f0 + 1) && !is_data; ++f) {
                is_data = cn >= frame_chips[f] && cn < frame_chips[f] + u;
            }
            if (!is_data) continue;
            const NetComplex* hn;
            if (cn == c) {
                hn = h_col_.data();
            } else {
                interpolate_h(static_cast<double>(cn), h_nb_.data());
                hn = h_nb_.data();
            }
            const int off = static_cast<int>(2 * (cn - c));
            for (int i = 0; i < win_len; ++i) {
                const int kk = i - off;
                h_shift_[static_cast<std::size_t>(i)] = kk >= 0 && kk < win_len ? hn[kk] : NetComplex{};
            }
            for (int i = 0; i < win_len; ++i) {
                const NetComplex hs = h_shift_[static_cast<std::size_t>(i)];
                if (hs == NetComplex{}) continue;
                for (int j = 0; j < win_len; ++j) {
                    NetComplex& r = r_mat_[static_cast<std::size_t>((i * win_len) + j)];
                    r = r + ((es_ * hs) * conj(h_shift_[static_cast<std::size_t>(j)]));
                }
            }
        }
        std::copy(h_col_.begin(), h_col_.end(), z_vec_.begin());
        NetComplex mu{};
        NetComplex xhat{};
        if (solve_hermitian(r_mat_.data(), win_len, z_vec_.data(), win_len)) {
            for (int i = 0; i < win_len; ++i) {
                const std::int64_t hc = (2 * c) + l_min + i;
                const NetComplex y =
                    hc >= hc0 && hc < hc0 + n ? ring_[hc - hc0] - probes_only[hc - hc0] : NetComplex{};
                mu = mu + (conj(h_col_[static_cast<std::size_t>(i)]) * z_vec_[static_cast<std::size_t>(i)]);
                xhat = xhat + (conj(z_vec_[static_cast<std::size_t>(i)]) * y);
            }
            mu = mu * es_;
            xhat = xhat * es_;
        }
        const double mu_r = std::clamp(mu.re, 1e-6, 1 - 1e-6);
        proj_[d] = xhat / mu_r;
        gram_t_[d] = mu_r / (es_ * (1 - mu_r));
    }
    sigma2_ = 1.0;
}

void MfbBlockDecoder::build_llrs() noexcept {
    const int u = mode_.u;
    const std::size_t symbols = static_cast<std::size_t>(il_.frames) * static_cast<std::size_t>(u);
    for (std::size_t d = 0; d < symbols; ++d) {
        const int pos = static_cast<int>(d % static_cast<std::size_t>(u));
        for (int q = 0; q < points_; ++q) {
            const NetComplex cq = wire(q, pos);
            const double dr = proj_[d].re - cq.re;
            const double di = proj_[d].im - cq.im;
            metric_[static_cast<std::size_t>(q)] = ((dr * dr) + (di * di)) * gram_t_[d] / sigma2_;
        }
        for (int bb = 0; bb < bps_; ++bb) {
            double m0 = std::numeric_limits<double>::max();
            double m1 = std::numeric_limits<double>::max();
            for (int q = 0; q < points_; ++q) {
                if (((q >> (bps_ - 1 - bb)) & 1) != 0) {
                    m1 = std::min(m1, metric_[static_cast<std::size_t>(q)]);
                } else {
                    m0 = std::min(m0, metric_[static_cast<std::size_t>(q)]);
                }
            }
            llrs_[(d * static_cast<std::size_t>(bps_)) + static_cast<std::size_t>(bb)] = static_cast<float>(m1 - m0);
        }
    }
}

// SISO per-bit posteriors to per-symbol expectations (independent bits).
void MfbBlockDecoder::set_decisions_soft() noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    interleaver_.deinterleave(llrs_, soft_punctured_);
    depuncture_soft(*puncture_, {soft_punctured_, size}, {soft_mother_, 2 * input});
    siso_->decode({soft_mother_, 2 * input}, {soft_mother_post_, 2 * input});
    apply_puncture_soft(*puncture_, {soft_mother_post_, 2 * input}, {soft_punctured_, size});
    interleaver_.interleave(soft_punctured_, soft_wire_);
    const int u = mode_.u;
    const std::size_t symbols = static_cast<std::size_t>(il_.frames) * static_cast<std::size_t>(u);
    for (std::size_t d = 0; d < symbols; ++d) {
        for (int bb = 0; bb < bps_; ++bb) {
            const double l = static_cast<double>(std::clamp(soft_wire_[(d * static_cast<std::size_t>(bps_)) + static_cast<std::size_t>(bb)], -30.0f, 30.0f));
            p0_[static_cast<std::size_t>(bb)] = 1.0 / (1.0 + std::exp(-l));
        }
        const int pos = static_cast<int>(d % static_cast<std::size_t>(u));
        NetComplex ex{};
        for (int m = 0; m < points_; ++m) {
            double pm = 1.0;
            for (int bb = 0; bb < bps_; ++bb) {
                const bool one = ((m >> (bps_ - 1 - bb)) & 1) != 0;
                pm *= one ? 1.0 - p0_[static_cast<std::size_t>(bb)] : p0_[static_cast<std::size_t>(bb)];
            }
            ex = ex + (pm * wire(m, pos));
        }
        decisions_[d] = ex;
    }
}

// ------------------------------------------------------------------ linear algebra

void MfbBlockDecoder::clear_normal(int len) noexcept {
    for (int i = 0; i < len; ++i) {
        rhs_[static_cast<std::size_t>(i)] = {};
        for (int j = 0; j < len; ++j) gram_[static_cast<std::size_t>((i * win_len) + j)] = {};
    }
}

void MfbBlockDecoder::accumulate_row(NetComplex row, int len) noexcept {
    for (int i = 0; i < len; ++i) {
        const NetComplex pi = conj(phi_row_[static_cast<std::size_t>(i)]);
        rhs_[static_cast<std::size_t>(i)] = rhs_[static_cast<std::size_t>(i)] + (pi * row);
        for (int j = 0; j < len; ++j) {
            NetComplex& g = gram_[static_cast<std::size_t>((i * win_len) + j)];
            g = g + (pi * phi_row_[static_cast<std::size_t>(j)]);
        }
    }
}

bool MfbBlockDecoder::solve_ridged(int len, double ridge_scale) noexcept {
    double trace = 0;
    for (int i = 0; i < len; ++i) trace += gram_[static_cast<std::size_t>((i * win_len) + i)].re;
    const double ridge = std::max(1e-9, ridge_scale * trace / len);
    for (int i = 0; i < len; ++i) gram_[static_cast<std::size_t>((i * win_len) + i)].re += ridge;
    return solve_hermitian(gram_.data(), win_len, rhs_.data(), len);
}

// Gaussian elimination with partial pivoting on magnitude, as the C# SolveHermitian.
bool MfbBlockDecoder::solve_hermitian(NetComplex* a, int stride, NetComplex* b, int n) noexcept {
    auto at = [a, stride](int r, int c) -> NetComplex& { return a[(r * stride) + c]; };
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (magnitude(at(r, col)) > magnitude(at(pivot, col))) pivot = r;
        }
        if (magnitude(at(pivot, col)) < 1e-12) return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c) std::swap(at(col, c), at(pivot, c));
            std::swap(b[col], b[pivot]);
        }
        for (int r = col + 1; r < n; ++r) {
            const NetComplex factor = at(r, col) / at(col, col);
            if (factor == NetComplex{}) continue;
            for (int c = col; c < n; ++c) at(r, c) = at(r, c) - (factor * at(col, c));
            b[r] = b[r] - (factor * b[col]);
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        NetComplex acc = b[r];
        for (int c = r + 1; c < n; ++c) acc = acc - (at(r, c) * b[c]);
        b[r] = acc / at(r, r);
    }
    return true;
}

double MfbBlockDecoder::price(const std::uint8_t* info, const std::int64_t* frame_chips) noexcept {
    set_decisions_from(info);
    return recon_residual(frame_chips, last_hc0_, last_n_, l_min_);
}

}  // inline namespace

}  // namespace m0lte::ms110d
