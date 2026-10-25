// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The turbo stage of the equaliser receive path (the C# Ms110dDemodulator, 0.2.1): the turbo
// re-equalisation loop run at the end of each block (TurboReequalize, TurboReequalizeSoft and
// TurboCore), the frozen-probe pass and the two salvage rungs for 8PSK (TurboFrozenProbePass,
// TrySalvageRevert, TrySalvageRelock), and the block helpers they share. Only the shipped paths
// are here: the C#'s instruments (genie, oracle, truth injection, probe and pair diagnostics,
// the consensus and alias-null variants, all off by default) are left out. See
// docs/dev/dfe-receiver.md.

#include "dfe_common.hpp"
#include "dfe_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

using dsp::ComplexF;
using dfe_detail::cf;
using dfe_detail::clampf;
using dfe_detail::eq;
using dfe_detail::psk8;
using dfe_detail::symbol_to_tribit8;

constexpr int interp_half = 4;
constexpr int segments = 4;
constexpr int turbo_iterations = 24;

// Descrambled-domain constellations for the chain BCJR: BPSK's labels are the indices; QPSK's
// points sit on the axes (Table D-IV di-bit -> ring 0, 2, 6, 4), so ring order 0, 2, 4, 6 carries
// labels 0, 1, 3, 2.
constexpr std::array<tables::Complex32, 2> turbo_bpsk{tables::psk8[0], tables::psk8[4]};
constexpr std::array<tables::Complex32, 4> turbo_qpsk{tables::psk8[0], tables::psk8[2], tables::psk8[4], tables::psk8[6]};
constexpr std::array<std::uint8_t, 4> turbo_qpsk_labels{0, 1, 3, 2};

int turbo_bits_per_symbol(Modulation m) noexcept {
    switch (m) {
        case Modulation::Bpsk: return 1;
        case Modulation::Qpsk: return 2;
        case Modulation::Psk8: return 3;
        default: return 4;
    }
}

struct TurboConstellation {
    std::span<const tables::Complex32> points;
    std::span<const std::uint8_t> labels;  // empty: label = index
};

TurboConstellation turbo_constellation(Modulation m) noexcept {
    switch (m) {
        case Modulation::Bpsk: return {turbo_bpsk, {}};
        case Modulation::Qpsk: return {turbo_qpsk, turbo_qpsk_labels};
        case Modulation::Psk8: return {tables::psk8, symbol_to_tribit8};
        default: return {tables::qam16, {}};
    }
}

// P(bit = 0) from an LLR under the positive-means-0 convention (the C# Sigmoid, in float).
float sigmoid(float llr) noexcept {
    return llr >= 0.0f ? 1.0f / (1.0f + std::exp(-llr)) : std::exp(llr) / (1.0f + std::exp(llr));
}

// log(1 + e^x), the C# Softplus.
float softplus(float x) noexcept {
    if (x > 20.0f) return x;
    return x < -20.0f ? 0.0f : std::log(1.0f + std::exp(x));
}

inline ComplexF from_eq(EqSample v) noexcept { return cf(v); }

}  // namespace

// ------------------------------------------------------------------ block helpers

// The C# Ms110dFraming.DecodeBlock: wire-order soft values to info bits.
void DfeReceiver::decode_block(const float* llrs, std::uint8_t* info) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    interleaver_.deinterleave(llrs, deinterleaved_);
    depuncture_soft(*puncture_, {deinterleaved_, size}, {mother_, 2 * input});
    viterbi_->decode({mother_, 2 * input}, {info, input});
}

// The C# Ms110dFraming.EncodeBlock: info bits to the fetched (wire-order) bits.
void DfeReceiver::encode_block(const std::uint8_t* info, std::uint8_t* fetched) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    fec::encode_tail_biting(*code_, {info, input}, {coded_, 2 * input});
    apply_puncture(*puncture_, {coded_, 2 * input}, {punctured_bits_, size});
    interleaver_.interleave(punctured_bits_, fetched);
}

// The turbo pass re-reads the whole block; one that has outlived the ring would train on
// overwritten samples. The ring is sized so that this always holds for the interleavers
// configured (as the C#'s 65536 sample ring always does for the 3 kHz set); it is the backstop.
bool DfeReceiver::block_samples_resident() const noexcept {
    const double oldest =
        (chip0_ + (2.0 * static_cast<double>(block_frame_chips_[0])) + tau_) - nff_ - interp_half;
    return oldest > static_cast<double>(stream_->written() - static_cast<std::int64_t>(stream_->size()));
}

// Hard labels on iteration 0, soft (SISO) feedback after, until two decodes in a row agree.
bool DfeReceiver::run_turbo_loop(std::uint8_t* info, std::uint8_t* prev_info, bool& aborted) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    aborted = false;
    for (int iter = 0; iter < turbo_iterations; ++iter) {
        if (iter == 0) {
            turbo_reequalize(info);
        } else {
            turbo_reequalize_soft();
        }
        if (block_llr_count_ != size) {
            aborted = true;  // a partial re-equalisation: the current decode stands
            return false;
        }
        std::memcpy(prev_info, info, input);
        decode_block(block_llrs_, info);
        if (std::memcmp(info, prev_info, input) == 0) return true;
    }
    return false;
}

// The C# TrySalvageRevert: a label-free frozen-probe re-detection of the block, then a fresh loop
// from that seed. True with the fixed point in `info`; false leaves `info` scribbled.
bool DfeReceiver::try_salvage_revert(std::uint8_t* info, std::uint8_t* prev_info) noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    turbo_frozen_probe_pass();
    if (block_llr_count_ != size) return false;
    decode_block(block_llrs_, info);
    bool aborted = false;
    return run_turbo_loop(info, prev_info, aborted);
}

// The second rung: the same, with the frozen pass offering the late-lock geometry.
bool DfeReceiver::try_salvage_relock(std::uint8_t* info, std::uint8_t* prev_info) noexcept {
    frozen_relock_active_ = true;
    const bool result = try_salvage_revert(info, prev_info);
    frozen_relock_active_ = false;
    return result;
}

// ------------------------------------------------------------------ labels

// Hard labels: re-encode the decode and run the core on the exact expected wire symbols.
void DfeReceiver::turbo_reequalize(const std::uint8_t* info) noexcept {
    encode_block(info, fetched_);
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const int bps = turbo_bits_per_symbol(mode_.modulation);
    std::size_t bit = 0;
    for (int f = 0; f < il_.frames; ++f) {
        scrambler_.reset();
        for (int u = 0; u < mode_.u; ++u) {
            int number = 0;
            for (int b = 0; b < bps; ++b) number = (number << 1) | (bit < size ? fetched_[bit++] : 0);
            ComplexF x;
            switch (mode_.modulation) {
                case Modulation::Bpsk: x = psk8(scrambler_.next_psk(number == 0 ? 0 : 4)); break;
                case Modulation::Qpsk: {
                    const int ring = number == 0 ? 0 : number == 1 ? 2 : number == 3 ? 4 : 6;
                    x = psk8(scrambler_.next_psk(ring));
                    break;
                }
                case Modulation::Psk8:
                    x = psk8(scrambler_.next_psk(tables::transcode_8psk[static_cast<std::size_t>(number & 7)]));
                    break;
                default:
                    x = cf(tables::qam16[static_cast<std::size_t>(scrambler_.next_qam(number, 4))]);
                    break;
            }
            turbo_expected_[(static_cast<std::size_t>(f) * static_cast<std::size_t>(mode_.u)) + static_cast<std::size_t>(u)] = x;
        }
    }
    turbo_core(turbo_expected_, nullptr, nullptr, false);
}

// Soft labels: a SISO pass over the outer code turns the current soft values into per-symbol
// expectations E[x] and variances, and the code's extrinsics become the chain BCJR's priors.
void DfeReceiver::turbo_reequalize_soft() noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    interleaver_.deinterleave(block_llrs_, soft_punctured_);
    depuncture_soft(*puncture_, {soft_punctured_, size}, {soft_mother_, 2 * input});
    siso_->decode({soft_mother_, 2 * input}, {soft_mother_post_, 2 * input});
    apply_puncture_soft(*puncture_, {soft_mother_post_, 2 * input}, {soft_punctured_, size});
    interleaver_.interleave(soft_punctured_, soft_wire_llrs_);

    // Extrinsics (posterior less channel input, mother domain) to wire order.
    for (std::size_t i = 0; i < 2 * input; ++i) soft_mother_[i] = soft_mother_post_[i] - soft_mother_[i];
    apply_puncture_soft(*puncture_, {soft_mother_, 2 * input}, {soft_punctured_, size});
    interleaver_.interleave(soft_punctured_, soft_wire_ext_);

    const int bps = turbo_bits_per_symbol(mode_.modulation);
    const bool qam = mode_.modulation == Modulation::Qam16;
    std::size_t bit = 0;
    float p0[4] = {0, 0, 0, 0};
    for (int f = 0; f < il_.frames; ++f) {
        scrambler_.reset();
        for (int u = 0; u < mode_.u; ++u) {
            for (int b = 0; b < bps; ++b) {
                p0[b] = sigmoid(bit < size ? soft_wire_llrs_[bit++] : std::numeric_limits<float>::max());
            }
            const int nibble = qam ? scrambler_.next_qam(0, 4) : 0;
            ComplexF e{0, 0};
            float e2 = 0.0f;
            for (int t = 0; t < (1 << bps); ++t) {
                float pt = 1.0f;
                for (int b = 0; b < bps; ++b) {
                    const float pz = p0[b];
                    pt *= ((t >> (bps - 1 - b)) & 1) == 0 ? pz : 1.0f - pz;
                }
                if (qam) {
                    const ComplexF w = cf(tables::qam16[static_cast<std::size_t>(t ^ nibble)]);
                    e = e + (w * pt);
                    e2 += dsp::mag2(w) * pt;
                    continue;
                }
                int ring;
                switch (mode_.modulation) {
                    case Modulation::Bpsk: ring = t == 0 ? 0 : 4; break;
                    case Modulation::Qpsk: ring = t == 0 ? 0 : t == 1 ? 2 : t == 3 ? 4 : 6; break;
                    default: ring = tables::transcode_8psk[static_cast<std::size_t>(t & 7)]; break;
                }
                e = e + (psk8(ring) * pt);
            }
            const std::size_t idx = (static_cast<std::size_t>(f) * static_cast<std::size_t>(mode_.u)) + static_cast<std::size_t>(u);
            if (qam) {
                soft_expected_[idx] = e;
                soft_var_[idx] = std::max(0.0f, e2 - dsp::mag2(e));
            } else {
                soft_expected_[idx] = e * psk8(scrambler_.next_psk(0));
                soft_var_[idx] = std::max(0.0f, 1.0f - dsp::mag2(e));
            }
        }
    }
    turbo_core(soft_expected_, soft_var_, soft_wire_ext_, true);
}

// ------------------------------------------------------------------ the core

// Per frame: a channel-shortening re-solve of the feed-forward filter on the expected symbols,
// per-segment main and echo gains, the scrambler-exact echo span, the noise floor (with the
// per-segment spike pricing), then the chain BCJR. Taps and the training accumulation are put
// back afterwards: the turbo pass changes nothing the tracking uses.
void DfeReceiver::turbo_core(const ComplexF* expected_all, const float* symbol_var, const float* wire_ext,
                             bool allow_pair) noexcept {
    const Mode& mode = mode_;
    const int U = mode.u;
    const int K = mode.k;
    const int fb = nfb_;
    const int n_taps = nff_ + nfb_;
    const std::span<EqSample> saved{turbo_saved_taps_.data(), static_cast<std::size_t>(n_taps)};
    dfe_.snapshot_taps(saved);
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> past{past_.data(), static_cast<std::size_t>(fb)};
    const std::span<const float> past_var{past_var_.data(), static_cast<std::size_t>(fb)};
    const int bps = turbo_bits_per_symbol(mode.modulation);
    const TurboConstellation tc = turbo_constellation(mode.modulation);
    const int m = static_cast<int>(tc.points.size());
    const bool qam = mode.modulation == Modulation::Qam16;
    block_llr_count_ = 0;

    auto abort_core = [&]() {
        dfe_.load_taps(saved);
        dfe_.begin_training();
    };

    for (int f = 0; f < il_.frames; ++f) {
        const std::int64_t frame_chip = block_frame_chips_[static_cast<std::size_t>(f)];
        const ComplexF* expected = expected_all + (static_cast<std::size_t>(f) * static_cast<std::size_t>(U));
        const float* expected_var = symbol_var == nullptr ? nullptr : symbol_var + (static_cast<std::size_t>(f) * static_cast<std::size_t>(U));
        if (qam) {
            for (int u = 0; u < U; ++u) {
                x2_[static_cast<std::size_t>(u)] = dsp::mag2(expected[u]) + (symbol_var == nullptr ? 0.0f : expected_var[u]);
            }
        }
        const std::int64_t base = static_cast<std::int64_t>(block_index_) * il_.frames;
        const ComplexF* preceding_probe = probe(pattern_.boundary(base + f - 1));

        // The shortening solve's rows: the feedback columns carry the wire-domain history (the
        // probe tail before u = 0).
        dfe_.begin_training();
        for (int u = 0; u < U; ++u) {
            if (!have_samples_for_chip(static_cast<double>(frame_chip + u + 2))) {
                abort_core();
                return;
            }
            fill_window(static_cast<double>(frame_chip + u), window_.data());
            for (int j = 0; j < fb; ++j) {
                const int idx = u - 1 - j;
                past_[static_cast<std::size_t>(j)] = eq(idx >= 0 ? expected[idx] : preceding_probe[K + idx]);
            }
            if (symbol_var == nullptr) {
                dfe_.add_training_row(window, past, eq(expected[u]), 1.0f);
            } else {
                for (int j = 0; j < fb; ++j) {
                    const int idx = u - 1 - j;
                    past_var_[static_cast<std::size_t>(j)] = idx >= 0 ? expected_var[idx] : 0.0f;
                }
                dfe_.add_training_row(window, past, past_var, eq(expected[u]), 1.0f);
            }
        }
        if (qam) {
            // QAM16: the bounding probes' rows keep a label-free truth floor in the solve.
            const ComplexF* following_probe = probe(pattern_.boundary(base + f));
            for (int p = 0; p < 2; ++p) {
                const ComplexF* pr = p == 0 ? preceding_probe : following_probe;
                const std::int64_t probe_chip = p == 0 ? frame_chip - K : frame_chip + U;
                for (int i = fb; i < K; ++i) {
                    if (!have_samples_for_chip(static_cast<double>(probe_chip + i + 2))) continue;
                    fill_window(static_cast<double>(probe_chip + i), window_.data());
                    for (int j = 0; j < fb; ++j) past_[static_cast<std::size_t>(j)] = eq(pr[i - 1 - j]);
                    dfe_.add_training_row(window, past, eq(pr[i]), 1.0f);
                }
            }
        }
        const TirSolve tir = dfe_.solve_training_tir(track_ridge_, 0.0f, std::min(fb, U / 2), allow_pair, 0);

        // The scrambler, and the re-solved feed-forward filter applied without feedback.
        scrambler_.reset();
        for (int u = 0; u < U; ++u) {
            if (qam) {
                nibbles_[static_cast<std::size_t>(u)] = scrambler_.next_qam(0, 4);
            } else {
                rotors_[static_cast<std::size_t>(u)] = psk8(scrambler_.next_psk(0));
            }
        }
        std::fill(past_.begin(), past_.begin() + fb, EqSample{});
        for (int u = 0; u < U; ++u) {
            fill_window(static_cast<double>(frame_chip + u), window_.data());
            rx_wire_[static_cast<std::size_t>(u)] = from_eq(dfe_.equalise(window, past));
        }

        // Per-segment main gain.
        const int seg_len = (U + segments - 1) / segments;
        ComplexF seg_h1[segments];
        ComplexF seg_h2[segments];
        ComplexF seg_h2b[segments];
        float seg_centre[segments];
        ComplexF h1_avg{0, 0};
        for (int s = 0; s < segments; ++s) {
            ComplexF acc{0, 0};
            float den = 0.0f;
            const int start = s * seg_len;
            const int end = std::min(U, start + seg_len);
            for (int u = start; u < end; ++u) {
                acc = acc + (rx_wire_[static_cast<std::size_t>(u)] * dsp::conj(expected[u]));
                if (qam) den += x2_[static_cast<std::size_t>(u)];
            }
            seg_h1[s] = acc * (qam ? 1.0f / std::max(1e-3f, den) : 1.0f / static_cast<float>(std::max(1, end - start)));
            seg_centre[s] = 0.5f * static_cast<float>(start + end - 1);
            h1_avg = h1_avg + seg_h1[s];
        }
        h1_avg = h1_avg * (1.0f / segments);

        int delay;
        ComplexF h2_avg;
        int delay2 = 0;
        ComplexF h2b{0, 0};
        const bool seg_echo = tir.lag > 0;
        if (tir.lag > 0) {
            // The shortening solve left the echo at this lag: estimate it there, per segment too.
            delay = tir.lag;
            ComplexF acc{0, 0};
            float den_avg = 0.0f;
            for (int u = delay; u < U; ++u) {
                acc = acc + ((rx_wire_[static_cast<std::size_t>(u)] - (h1_avg * expected[u])) * dsp::conj(expected[u - delay]));
                if (qam) den_avg += x2_[static_cast<std::size_t>(u - delay)];
            }
            h2_avg = acc * (qam ? 1.0f / std::max(1e-3f, den_avg) : 1.0f / static_cast<float>(U - delay));
            for (int s = 0; s < segments; ++s) {
                ComplexF seg_acc{0, 0};
                float seg_den = 0.0f;
                const int start = std::max(delay, s * seg_len);
                const int end = std::min(U, (s * seg_len) + seg_len);
                const int count = end - start;
                for (int u = start; u < end; ++u) {
                    seg_acc = seg_acc + ((rx_wire_[static_cast<std::size_t>(u)] - (h1_avg * expected[u])) * dsp::conj(expected[u - delay]));
                    if (qam) seg_den += x2_[static_cast<std::size_t>(u - delay)];
                }
                seg_h2[s] = count > 0 ? seg_acc * (qam ? 1.0f / std::max(1e-3f, seg_den) : 1.0f / static_cast<float>(count)) : h2_avg;
            }

            // The straddle pair: the adjacent-lag coefficient on the doubly subtracted residual,
            // cancelled softly before the chains.
            if (tir.lag2 > 0) {
                const int start2 = std::max(delay, tir.lag2);
                ComplexF acc_b{0, 0};
                float den_b = 0.0f;
                for (int u = start2; u < U; ++u) {
                    const ComplexF r = (rx_wire_[static_cast<std::size_t>(u)] - (h1_avg * expected[u])) - (h2_avg * expected[u - delay]);
                    acc_b = acc_b + (r * dsp::conj(expected[u - tir.lag2]));
                    if (qam) den_b += x2_[static_cast<std::size_t>(u - tir.lag2)];
                }
                h2b = acc_b * (qam ? 1.0f / std::max(1e-3f, den_b) : 1.0f / static_cast<float>(std::max(1, U - start2)));
                delay2 = tir.lag2;
                for (int s = 0; s < segments; ++s) {
                    ComplexF seg_acc{0, 0};
                    float seg_den = 0.0f;
                    const int start = std::max(start2, s * seg_len);
                    const int end = std::min(U, (s * seg_len) + seg_len);
                    const int count = end - start;
                    for (int u = start; u < end; ++u) {
                        const ComplexF r = (rx_wire_[static_cast<std::size_t>(u)] - (h1_avg * expected[u])) - (h2_avg * expected[u - delay]);
                        seg_acc = seg_acc + (r * dsp::conj(expected[u - tir.lag2]));
                        if (qam) seg_den += x2_[static_cast<std::size_t>(u - tir.lag2)];
                    }
                    seg_h2b[s] = count > 0 ? seg_acc * (qam ? 1.0f / std::max(1e-3f, seg_den) : 1.0f / static_cast<float>(count)) : h2b;
                }
            }
        } else {
            // The full-inversion solve: a free echo search on the residuals, up to 24 symbols
            // (inside the probe), with a significance floor.
            const int max_lag = std::min(std::min(24, U / 2), K);
            delay = 1;
            h2_avg = {0, 0};
            for (int lag = 1; lag <= max_lag; ++lag) {
                ComplexF acc{0, 0};
                float den = 0.0f;
                for (int u = lag; u < U; ++u) {
                    acc = acc + ((rx_wire_[static_cast<std::size_t>(u)] - (h1_avg * expected[u])) * dsp::conj(expected[u - lag]));
                    if (qam) den += x2_[static_cast<std::size_t>(u - lag)];
                }
                const ComplexF h2 = acc * (qam ? 1.0f / std::max(1e-3f, den) : 1.0f / static_cast<float>(U - lag));
                if (dsp::mag2(h2) > dsp::mag2(h2_avg)) {
                    h2_avg = h2;
                    delay = lag;
                }
            }
            if (dsp::mag2(h2_avg) < 0.04f * dsp::mag2(h1_avg)) {
                h2_avg = {0, 0};
                delay = 1;
            }
        }

        // The descrambled-domain model: z[u] = y[u] conj(r(u)) keeps h1, and the scrambler
        // enters the echo coefficient. QAM16 stays in the wire domain.
        for (int c = 0; c < delay; ++c) preceding_[static_cast<std::size_t>(c)] = preceding_probe[(K - delay) + c];
        float residual = 0.0f;
        float seg_resid[segments] = {0, 0, 0, 0};
        int seg_resid_count[segments] = {0, 0, 0, 0};
        for (int u = 0; u < U; ++u) {
            const std::size_t uu = static_cast<std::size_t>(u);
            const int sn = std::min(segments - 1, u / seg_len);
            const float resid_before = residual;
            int s = sn;
            int ia;
            int ib;
            float t;
            const float uf = static_cast<float>(u);
            if (uf <= seg_centre[0]) {
                ia = ib = 0;
                t = 0.0f;
            } else if (uf >= seg_centre[segments - 1]) {
                ia = ib = segments - 1;
                t = 0.0f;
            } else {
                if (uf < seg_centre[s]) --s;
                ia = s;
                ib = s + 1;
                t = (uf - seg_centre[s]) / (seg_centre[s + 1] - seg_centre[s]);
            }
            const ComplexF h1u = ia == ib ? seg_h1[ia] : (seg_h1[ia] * (1.0f - t)) + (seg_h1[ib] * t);
            const ComplexF h2u = seg_echo ? (ia == ib ? seg_h2[ia] : (seg_h2[ia] * (1.0f - t)) + (seg_h2[ib] * t)) : h2_avg;
            if (delay2 > 0) {
                const ComplexF h2bu = ia == ib ? seg_h2b[ia] : (seg_h2b[ia] * (1.0f - t)) + (seg_h2b[ib] * t);
                const ComplexF src_b = u >= delay2 ? expected[u - delay2] : preceding_probe[K + (u - delay2)];
                rx_wire_[uu] = rx_wire_[uu] - (h2bu * src_b);
            }
            h1_span_[uu] = h1u;
            rx_desc_[uu] = qam ? rx_wire_[uu] : rx_wire_[uu] * dsp::conj(rotors_[uu]);
            const ComplexF echo_wire = u >= delay ? expected[u - delay] : preceding_[uu];
            h2_span_[uu] = qam ? h2u
                               : u >= delay ? (h2u * rotors_[static_cast<std::size_t>(u - delay)]) * dsp::conj(rotors_[uu])
                                            : h2u * dsp::conj(rotors_[uu]);
            const ComplexF predicted = (h1u * expected[u]) + (h2u * echo_wire);
            residual += dsp::mag2(rx_wire_[uu] - predicted);
            if (symbol_var != nullptr) {
                // The EM-consistent noise for soft labels: |z - h E[x]|^2 + |h|^2 Var[x].
                residual += dsp::mag2(h1u) * expected_var[u];
                if (u >= delay) residual += dsp::mag2(h2u) * expected_var[u - delay];
                if (delay2 > 0 && u >= delay2) residual += dsp::mag2(h2b) * expected_var[u - delay2];
            }
            seg_resid[sn] += residual - resid_before;
            ++seg_resid_count[sn];
        }

        // The noise per complex dimension; per-segment spikes above the frame's floor beyond their
        // own 3 sigma chi-squared band are priced locally (the shipped "spikeup").
        const float noise_var = std::max(0.5f * residual / static_cast<float>(U), 1e-6f);
        float seg_nv[segments];
        float seg_price[segments];
        bool engaged = false;
        for (int s = 0; s < segments; ++s) {
            seg_nv[s] = seg_resid_count[s] > 0
                            ? std::max(0.5f * seg_resid[s] / static_cast<float>(seg_resid_count[s]), 1e-6f)
                            : noise_var;
        }
        // The C# TurboNsegMode: "spikeup" (shipped, upward only), "spike2s" (both ways) or "off"
        // (the frame constant throughout).
        if (config_.noise_pricing != DfeNoisePricing::Off) {
            const bool two_sided = config_.noise_pricing == DfeNoisePricing::Spike2s;
            for (int s = 0; s < segments; ++s) {
                const float thr = seg_resid_count[s] > 0
                                      ? std::exp(3.0f / std::sqrt(static_cast<float>(seg_resid_count[s])))
                                      : std::numeric_limits<float>::max();
                const bool spike = seg_nv[s] > noise_var * thr || (two_sided && seg_nv[s] < noise_var / thr);
                seg_price[s] = spike ? seg_nv[s] : noise_var;
                engaged = engaged || spike;
            }
        }
        if (engaged) {
            for (int u = 0; u < U; ++u) {
                int s = std::min(segments - 1, u / seg_len);
                const float uf = static_cast<float>(u);
                float v;
                if (uf <= seg_centre[0]) {
                    v = seg_price[0];
                } else if (uf >= seg_centre[segments - 1]) {
                    v = seg_price[segments - 1];
                } else {
                    if (uf < seg_centre[s]) --s;
                    const float t = (uf - seg_centre[s]) / (seg_centre[s + 1] - seg_centre[s]);
                    v = (seg_price[s] * (1.0f - t)) + (seg_price[s + 1] * t);
                }
                nv_span_[static_cast<std::size_t>(u)] = v;
            }
        }

        // The outer code's extrinsics as per-symbol log priors (data-domain labels).
        const std::size_t bit_base = static_cast<std::size_t>(f) * static_cast<std::size_t>(U) * static_cast<std::size_t>(bps);
        if (wire_ext != nullptr) {
            float lp0[4];
            float lp1[4];
            for (int u = 0; u < U; ++u) {
                for (int b = 0; b < bps; ++b) {
                    const float ext = wire_ext[bit_base + (static_cast<std::size_t>(u) * static_cast<std::size_t>(bps)) + static_cast<std::size_t>(b)];
                    lp0[b] = -softplus(-ext);
                    lp1[b] = -softplus(ext);
                }
                for (int s = 0; s < m; ++s) {
                    int label = tc.labels.empty() ? s : tc.labels[static_cast<std::size_t>(s)];
                    if (qam) label = s ^ nibbles_[static_cast<std::size_t>(u)];
                    float sum = 0.0f;
                    for (int b = 0; b < bps; ++b) sum += ((label >> (bps - 1 - b)) & 1) == 0 ? lp0[b] : lp1[b];
                    log_priors_[static_cast<std::size_t>((u * m) + s)] = sum;
                }
            }
        }

        // The chain BCJR, in single precision as the C# (docs/dev/dfe-receiver.md).
        for (int u = 0; u < U; ++u) {
            const std::size_t uu = static_cast<std::size_t>(u);
            bcjr_rx_[uu] = {rx_desc_[uu].re, rx_desc_[uu].im};
            bcjr_h1_[uu] = {h1_span_[uu].re, h1_span_[uu].im};
            bcjr_h2_[uu] = {h2_span_[uu].re, h2_span_[uu].im};
        }
        for (int c = 0; c < delay; ++c) {
            bcjr_preceding_[static_cast<std::size_t>(c)] = {preceding_[static_cast<std::size_t>(c)].re, preceding_[static_cast<std::size_t>(c)].im};
        }
        const std::size_t n_llrs = static_cast<std::size_t>(U * bps);
        BasicChainBcjr<float>::Input in;
        in.rx = {bcjr_rx_.data(), static_cast<std::size_t>(U)};
        in.h1 = {bcjr_h1_.data(), static_cast<std::size_t>(U)};
        in.h2 = {bcjr_h2_.data(), static_cast<std::size_t>(U)};
        in.delay = delay;
        in.noise_var = noise_var;
        in.constellation = tc.points;
        in.bit_labels = tc.labels;
        in.bits_per_symbol = bps;
        in.preceding = {bcjr_preceding_.data(), static_cast<std::size_t>(delay)};
        if (wire_ext != nullptr) in.symbol_log_priors = {log_priors_.data(), static_cast<std::size_t>(U * m)};
        if (engaged) in.noise_var_per_symbol = {nv_span_.data(), static_cast<std::size_t>(U)};
        decoders_.chain->equalize(in, {bcjr_llrs_.data(), n_llrs});
        for (std::size_t i = 0; i < n_llrs; ++i) frame_llrs_[i] = bcjr_llrs_[i];

        if (qam) {
            // The chains gave wire-bit soft values; data bit = wire bit XOR scramble bit.
            for (int u = 0; u < U; ++u) {
                const int nib = nibbles_[static_cast<std::size_t>(u)];
                for (int b = 0; b < bps; ++b) {
                    if (((nib >> (bps - 1 - b)) & 1) != 0) {
                        float& v = frame_llrs_[static_cast<std::size_t>((u * bps) + b)];
                        v = -v;
                    }
                }
            }
        }
        if (wire_ext != nullptr) {
            // Posterior less prior: extrinsics only go back to the outer code.
            for (std::size_t i = 0; i < n_llrs; ++i) frame_llrs_[i] -= wire_ext[bit_base + i];
        }
        for (std::size_t i = 0; i < n_llrs; ++i) add_llr(frame_llrs_[i]);
    }

    dfe_.load_taps(saved);
    dfe_.begin_training();
}

// ------------------------------------------------------------------ the frozen probe pass

// Probe-only shortening rows for frame f, the feedback history wholly inside each probe;
// shift > 0 trains the same rows with the window advanced by that many chips (the late-lock
// geometry). Returns the rows added.
int DfeReceiver::frozen_probe_rows(int f, int shift) noexcept {
    const int K = mode_.k;
    const int fb = nfb_;
    const std::int64_t frame_chip = block_frame_chips_[static_cast<std::size_t>(f)];
    const std::int64_t base = static_cast<std::int64_t>(block_index_) * il_.frames;
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> past{past_.data(), static_cast<std::size_t>(fb)};
    dfe_.begin_training();
    int rows = 0;
    for (int p = 0; p < 2; ++p) {
        const ComplexF* pr = probe(pattern_.boundary(base + f + p - 1));
        const std::int64_t probe_chip = p == 0 ? frame_chip - K : frame_chip + mode_.u;
        for (int i = fb; i < K; ++i) {
            if (!have_samples_for_chip(static_cast<double>(probe_chip + i + shift + 2))) continue;
            fill_window(static_cast<double>(probe_chip + i + shift), window_.data());
            for (int j = 0; j < fb; ++j) past_[static_cast<std::size_t>(j)] = eq(pr[i - 1 - j]);
            dfe_.add_training_row(window, past, eq(pr[i]), 1.0f);
            ++rows;
        }
    }
    return rows;
}

// The C# TurboFrozenProbePass (8PSK and the other PSK modes): a label-free re-detection of the
// block. Per frame a probe-only shortening solve, feedback-free application, the main gain from
// the two probe anchors interpolated across the frame, the solve's own echo as the chain model
// (or, for a lag beyond half the probe period, the exact pre-cursor form), a probe-priced noise
// floor, and the late-lock geometry when the relock rung is active.
void DfeReceiver::turbo_frozen_probe_pass() noexcept {
    const Mode& mode = mode_;
    const int U = mode.u;
    const int K = mode.k;
    const int fb = nfb_;
    const int n_taps = nff_ + nfb_;
    const std::span<EqSample> saved{turbo_saved_taps_.data(), static_cast<std::size_t>(n_taps)};
    dfe_.snapshot_taps(saved);
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> past{past_.data(), static_cast<std::size_t>(fb)};
    const int bps = turbo_bits_per_symbol(mode.modulation);
    const TurboConstellation tc = turbo_constellation(mode.modulation);
    block_llr_count_ = 0;
    const int probe_period = static_cast<int>(tables::find_probe_spec(K)->base.size());
    const int max_lag = std::min(fb, U / 2);

    auto restore = [&]() {
        dfe_.load_taps(saved);
        dfe_.begin_training();
    };

    for (int f = 0; f < il_.frames; ++f) {
        const std::int64_t frame_chip = block_frame_chips_[static_cast<std::size_t>(f)];
        const std::int64_t base = static_cast<std::int64_t>(block_index_) * il_.frames;
        const ComplexF* preceding_probe = probe(pattern_.boundary(base + f - 1));
        const ComplexF* following_probe = probe(pattern_.boundary(base + f));

        const int solve_rows = frozen_probe_rows(f, 0);
        if (solve_rows == 0) {
            restore();
            return;
        }
        TirSolve tir = dfe_.solve_training_tir(track_ridge_, 0.0f, max_lag, false, 0);
        int delay = std::max(1, tir.lag);
        ComplexF h2_wire = tir.lag > 0 ? cf(tir.coefficient) : ComplexF{0, 0};

        // Feedback-free application, the data span and the probe rows alike.
        std::fill(past_.begin(), past_.begin() + fb, EqSample{});
        for (int u = 0; u < U; ++u) {
            if (!have_samples_for_chip(static_cast<double>(frame_chip + u + 2))) {
                restore();
                return;
            }
            fill_window(static_cast<double>(frame_chip + u), window_.data());
            rx_wire_[static_cast<std::size_t>(u)] = from_eq(dfe_.equalise(window, past));
        }

        // Probe anchors, the echo removed with the solve's own coefficient, and the noise floor.
        ComplexF anchor[2];
        float anchor_pos[2];
        float noise_acc = 0.0f;
        int noise_rows = 0;
        auto probe_anchors = [&](int shift, const TirSolve& t, ComplexF* anc, float* pos, float& acc_out, int& rows_out) {
            for (int p = 0; p < 2; ++p) {
                const ComplexF* pr = p == 0 ? preceding_probe : following_probe;
                const std::int64_t probe_chip = p == 0 ? frame_chip - K : frame_chip + U;
                const float u_base = p == 0 ? static_cast<float>(-K) : static_cast<float>(U);
                const int first_row = t.lag > 0 ? t.lag : 0;
                ComplexF acc{0, 0};
                int n = 0;
                float pos_sum = 0.0f;
                for (int i = first_row; i < K; ++i) {
                    if (!have_samples_for_chip(static_cast<double>(probe_chip + i + shift + 2))) continue;
                    fill_window(static_cast<double>(probe_chip + i + shift), window_.data());
                    ComplexF y = from_eq(dfe_.equalise(window, past));
                    if (t.lag > 0) y = y - (cf(t.coefficient) * pr[i - t.lag]);
                    probe_y_[static_cast<std::size_t>((p * K) + n)] = y;
                    probe_idx_[static_cast<std::size_t>((p * K) + n)] = i;
                    acc = acc + (y * dsp::conj(pr[i]));
                    pos_sum += u_base + static_cast<float>(i);
                    ++n;
                }
                anc[p] = n > 0 ? acc * (1.0f / static_cast<float>(n)) : ComplexF{0, 0};
                pos[p] = n > 0 ? pos_sum / static_cast<float>(n)
                               : (p == 0 ? static_cast<float>(-K) * 0.5f : static_cast<float>(U) + (static_cast<float>(K) * 0.5f));
                for (int r = 0; r < n; ++r) {
                    const int i = probe_idx_[static_cast<std::size_t>((p * K) + r)];
                    const ComplexF resid = probe_y_[static_cast<std::size_t>((p * K) + r)] - (anc[p] * pr[i]);
                    acc_out += dsp::mag2(resid);
                    ++rows_out;
                }
            }
        };
        probe_anchors(0, tir, anchor, anchor_pos, noise_acc, noise_rows);
        float noise_var = std::max(noise_rows > 0 ? 0.5f * noise_acc / static_cast<float>(noise_rows) : 1e-2f, 1e-6f);

        // The late-lock offer: re-train with the window shifted by the accepted lag, solve only
        // the aliased pre-cursor lag, and keep the geometry with the decisively lower floor.
        int lock_shift = 0;
        if (frozen_relock_active_ && tir.lag >= 1 && tir.lag < probe_period / 2 && probe_period - tir.lag <= max_lag) {
            const int shift = tir.lag;
            const std::span<EqSample> causal{causal_taps_.data(), static_cast<std::size_t>(n_taps)};
            dfe_.snapshot_taps(causal);
            bool adopted = false;
            if (frozen_probe_rows(f, shift) == solve_rows) {
                const TirSolve s_tir = dfe_.solve_training_tir(track_ridge_, 0.0f, max_lag, false, probe_period - shift);
                if (s_tir.lag == probe_period - shift) {
                    std::fill(past_.begin(), past_.begin() + fb, EqSample{});
                    bool have = true;
                    for (int u = 0; u < U; ++u) {
                        if (!have_samples_for_chip(static_cast<double>(frame_chip + u + shift + 2))) {
                            have = false;
                            break;
                        }
                        fill_window(static_cast<double>(frame_chip + u + shift), window_.data());
                        rx_alt_[static_cast<std::size_t>(u)] = from_eq(dfe_.equalise(window, past));
                    }
                    if (have) {
                        ComplexF anchor_alt[2];
                        float anchor_pos_alt[2];
                        float alt_acc = 0.0f;
                        int alt_rows = 0;
                        probe_anchors(shift, s_tir, anchor_alt, anchor_pos_alt, alt_acc, alt_rows);
                        const float alt_var =
                            std::max(alt_rows > 0 ? 0.5f * alt_acc / static_cast<float>(alt_rows) : 1e-2f, 1e-6f);
                        const bool adopt = alt_rows > 0 && alt_var < 0.5f * noise_var;
                        ++block_.relock_offers;
                        if (adopt) ++block_.relock_adopted;
                        if (adopt) {
                            tir = s_tir;
                            delay = std::max(1, tir.lag);
                            h2_wire = cf(tir.coefficient);
                            std::copy(rx_alt_.begin(), rx_alt_.begin() + U, rx_wire_.begin());
                            anchor[0] = anchor_alt[0];
                            anchor[1] = anchor_alt[1];
                            anchor_pos[0] = anchor_pos_alt[0];
                            anchor_pos[1] = anchor_pos_alt[1];
                            noise_var = alt_var;
                            lock_shift = shift;
                            adopted = true;
                        }
                    }
                }
            }
            if (!adopted) dfe_.load_taps(causal);
        }

        // A lag beyond half the probe period is the pre-cursor folded through the periodic probe:
        // run the chains exactly on the pre-cursor structure.
        int chain_delay = delay;
        const ComplexF h2_chain = h2_wire;
        const bool alias_frame = tir.lag > probe_period / 2;
        const bool pre_cursor_frame = alias_frame && probe_period - tir.lag >= 1;
        if (pre_cursor_frame) chain_delay = probe_period - tir.lag;

        scrambler_.reset();
        for (int u = 0; u < U; ++u) rotors_[static_cast<std::size_t>(u)] = psk8(scrambler_.next_psk(0));
        for (int c = 0; c < chain_delay; ++c) {
            preceding_[static_cast<std::size_t>(c)] = preceding_probe[(K - chain_delay) + c];
        }
        const float span = std::max(1.0f, anchor_pos[1] - anchor_pos[0]);
        if (pre_cursor_frame) {
            const int d = chain_delay;
            for (int u = 0; u < d; ++u) {
                const std::int64_t chip = frame_chip - d + u + lock_shift;
                if (!have_samples_for_chip(static_cast<double>(chip + 2))) {
                    restore();
                    return;
                }
                fill_window(static_cast<double>(chip), window_.data());
                rx_desc_[static_cast<std::size_t>(u)] = from_eq(dfe_.equalise(window, past)) * dsp::conj(rotors_[static_cast<std::size_t>(u)]);
            }
            for (int u = d; u < U; ++u) {
                rx_desc_[static_cast<std::size_t>(u)] = rx_wire_[static_cast<std::size_t>(u - d)] * dsp::conj(rotors_[static_cast<std::size_t>(u)]);
            }
            const ComplexF c1 = cf(tir.coefficient);
            for (int u = 0; u < U; ++u) {
                const std::size_t uu = static_cast<std::size_t>(u);
                h1_span_[uu] = c1;
                const float t = clampf((static_cast<float>(u - d) - anchor_pos[0]) / span, 0.0f, 1.0f);
                const ComplexF h1w = (anchor[0] * (1.0f - t)) + (anchor[1] * t);
                h2_span_[uu] = u >= d ? (h1w * rotors_[static_cast<std::size_t>(u - d)]) * dsp::conj(rotors_[uu])
                                      : h1w * dsp::conj(rotors_[uu]);
            }
        } else {
            for (int u = 0; u < U; ++u) {
                const std::size_t uu = static_cast<std::size_t>(u);
                const float t = clampf((static_cast<float>(u) - anchor_pos[0]) / span, 0.0f, 1.0f);
                h1_span_[uu] = (anchor[0] * (1.0f - t)) + (anchor[1] * t);
                rx_desc_[uu] = rx_wire_[uu] * dsp::conj(rotors_[uu]);
                h2_span_[uu] = u >= chain_delay
                                   ? (h2_chain * rotors_[static_cast<std::size_t>(u - chain_delay)]) * dsp::conj(rotors_[uu])
                                   : h2_chain * dsp::conj(rotors_[uu]);
            }
        }

        for (int u = 0; u < U; ++u) {
            const std::size_t uu = static_cast<std::size_t>(u);
            bcjr_rx_[uu] = {rx_desc_[uu].re, rx_desc_[uu].im};
            bcjr_h1_[uu] = {h1_span_[uu].re, h1_span_[uu].im};
            bcjr_h2_[uu] = {h2_span_[uu].re, h2_span_[uu].im};
        }
        for (int c = 0; c < chain_delay; ++c) {
            bcjr_preceding_[static_cast<std::size_t>(c)] = {preceding_[static_cast<std::size_t>(c)].re, preceding_[static_cast<std::size_t>(c)].im};
        }
        const std::size_t n_llrs = static_cast<std::size_t>(U * bps);
        BasicChainBcjr<float>::Input in;
        in.rx = {bcjr_rx_.data(), static_cast<std::size_t>(U)};
        in.h1 = {bcjr_h1_.data(), static_cast<std::size_t>(U)};
        in.h2 = {bcjr_h2_.data(), static_cast<std::size_t>(U)};
        in.delay = chain_delay;
        in.noise_var = noise_var;
        in.constellation = tc.points;
        in.bit_labels = tc.labels;
        in.bits_per_symbol = bps;
        in.preceding = {bcjr_preceding_.data(), static_cast<std::size_t>(chain_delay)};
        decoders_.chain->equalize(in, {bcjr_llrs_.data(), n_llrs});
        for (std::size_t i = 0; i < n_llrs; ++i) add_llr(bcjr_llrs_[i]);
    }
    restore();
}

}  // inline namespace

}  // namespace m0lte::ms110d
