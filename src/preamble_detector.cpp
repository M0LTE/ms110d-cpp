// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "preamble_detector.hpp"

#include "dsp/complex_f.hpp"
#include "framing.hpp"
#include "preamble.hpp"
#include "tables.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace m0lte::ms110d {

namespace {

constexpr int chips_fixed = PreambleDetector::chips_per_fixed;
constexpr int chips_superframe = PreambleDetector::chips_per_superframe;
constexpr int interp_half = 4;  // 8 tap interpolator
constexpr double bins_hz[PreambleDetector::search_bins] = {-75, -50, -25, 0, 25, 50, 75};

using dsp::ComplexF;

inline ComplexF cadd(ComplexF a, ComplexF b) noexcept { return a + b; }
inline ComplexF csub(ComplexF a, ComplexF b) noexcept { return a - b; }
inline ComplexF cmul(ComplexF a, ComplexF b) noexcept { return a * b; }
inline ComplexF cscale(ComplexF a, float s) noexcept { return a * s; }
inline ComplexF cconj(ComplexF a) noexcept { return dsp::conj(a); }
inline float cmag2(ComplexF a) noexcept { return dsp::mag2(a); }
inline float cabs(ComplexF a) noexcept { return dsp::abs(a); }
inline float carg(ComplexF a) noexcept { return dsp::arg(a); }
inline ComplexF from_phase(float phase) noexcept { return dsp::from_phase(phase); }

inline ComplexF psk8(std::uint8_t symbol) noexcept {
    const tables::Complex32 v = tables::psk8[symbol];
    return {v.real(), v.imag()};
}

// The C# EstimateCarrierFit: carrier phase and frequency from per-group correlation phasors,
// by lag products (a faded group suppresses itself) instead of a phase unwrap.
bool estimate_carrier_fit(const ComplexF* groups, int count, double& slope_rad_per_group, double& intercept_rad) noexcept {
    slope_rad_per_group = 0;
    intercept_rad = 0;
    if (count < 2) return false;

    ComplexF lag1{0, 0};
    for (int j = 0; j + 1 < count; ++j) lag1 = cadd(lag1, cmul(groups[j + 1], cconj(groups[j])));
    if (static_cast<double>(cmag2(lag1)) < 1e-12) return false;

    double slope = static_cast<double>(carg(lag1));

    const int lag = std::min(8, count - 1);
    if (lag > 1) {
        ComplexF lag_l{0, 0};
        for (int j = 0; j + lag < count; ++j) lag_l = cadd(lag_l, cmul(groups[j + lag], cconj(groups[j])));
        if (static_cast<double>(cmag2(lag_l)) > 1e-12) {
            // Resolve the lag-L phase onto the branch nearest the lag-1 estimate.
            const double expected = slope * lag;
            double phi_l = static_cast<double>(carg(lag_l));
            phi_l += 2.0 * std::numbers::pi * std::nearbyint((expected - phi_l) / (2.0 * std::numbers::pi));
            slope = phi_l / lag;
        }
    }

    ComplexF anchor{0, 0};
    for (int j = 0; j < count; ++j) {
        anchor = cadd(anchor, cmul(groups[j], from_phase(static_cast<float>(-slope * j))));
    }
    if (static_cast<double>(cmag2(anchor)) < 1e-12) return false;

    slope_rad_per_group = slope;
    intercept_rad = static_cast<double>(carg(anchor));
    return true;
}

}  // namespace

// The C# InitializeDfe level rule (issues 7 and 17 in the C#). Every waveform with mini-probes
// scales the received signal to a fixed level, up or down, so the equaliser starts at the level it
// was tuned at: 0.25 for WN 1 and 2 (probe length 48), 0.5 for the other PSK waveforms. WN 8 keeps
// the older rule, 1.0 unless the level is under 0.04, then a boost to 0.12. The boost is capped at
// 256. The operands are the C#'s float constants, widened to double as the C# does.
float level_gain(int waveform, double level) noexcept {
    if (level <= 0) return 1.0f;
    const auto mode = mode_3k(waveform);
    if (!mode || mode->modulation == Modulation::Walsh) return 1.0f;
    constexpr double max_gain = static_cast<double>(256.0f);
    if (mode->modulation == Modulation::Qam16) {
        return level >= static_cast<double>(0.04f) ? 1.0f : static_cast<float>(std::min(static_cast<double>(0.12f) / level, max_gain));
    }
    const double target = mode->k == 48 ? static_cast<double>(0.25f) : static_cast<double>(0.5f);
    return static_cast<float>(std::min(target / level, max_gain));
}

bool PreambleDetector::configure(const PreambleDetectorConfig& config, RxStream& stream) noexcept {
    taps_ = 0;
    stream_ = nullptr;
    std::array<float, max_pulse_taps> pulse{};
    const std::size_t taps = design_pulse(config.roll_off, pulse);
    if (taps == 0 || !stream.configured() || stream.size() < ring_size) return false;
    taps_ = taps;
    stream_ = &stream;
    sync_threshold_ = config.sync_threshold;

    // The matched-filter references: the Fixed subsection (9 Walsh symbols, the M >= 2 form),
    // conjugated and counter-rotated by each grid bin's frequency.
    const auto fixed = Preamble::make(0, 2, 0, InterleaverKind::Short, 7);
    for (int b = 0; b < search_bins; ++b) {
        for (int i = 0; i < chips_fixed; ++i) {
            const double theta = 2.0 * std::numbers::pi * bins_hz[b] * i / tables::symbol_rate;
            const ComplexF r = cmul(cconj(psk8(fixed->chip(static_cast<std::size_t>(i)))), from_phase(-static_cast<float>(theta)));
            mf_reference_[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)] = r;
        }
    }
    reset();
    return true;
}

void PreambleDetector::reset() noexcept {
    comb_energy_ = {0, 0};
    best_metric_ = 0;
    best_start_ = -1;
    best_bin_ = 0;
    metric_ring_.fill(0);
    metric_bin_ring_.fill(0);
    state_ = State::Searching;
    count_read_ = false;
    tail_pending_ = false;
    votes_ = 0;
    alt_available_ = false;
    track_from_earliest_ = false;
    cand_pending_ = false;
    shadow_from_ = 0;
    event_ = {};
    stats_ = {};
}

std::complex<float> PreambleDetector::t2_sample(std::int64_t index) const noexcept {
    if (!t2_available(index)) return {0.0f, 0.0f};
    const ComplexF v = stream_->at(index);
    return {v.re, v.im};
}

bool PreambleDetector::take_detection(PreambleDetection& out) noexcept {
    if (state_ != State::Detected) return false;
    out = event_;
    state_ = State::Background;
    return true;
}

void PreambleDetector::resume_search() noexcept {
    if (taps_ == 0) return;
    back_to_search();
}

void PreambleDetector::back_to_search() noexcept {
    state_ = State::Searching;
    best_metric_ = 0;
    best_start_ = -1;
    count_read_ = false;
    tail_pending_ = false;
    alt_available_ = false;
    track_from_earliest_ = false;
    cand_pending_ = false;
}

bool PreambleDetector::background_step() noexcept {
    if (state_ != State::Background) return false;
    if (!cand_pending_) {
        search_step();
        return false;
    }
    // Read with the candidate's timing and carrier at unity gain; nothing else reads them now.
    switch (read_preamble()) {
        case Read::NotReady: return false;
        case Read::Rejected: reject_candidate(); return false;
        case Read::Locked: ++stats_.new_preambles; return true;
    }
    return false;
}

void PreambleDetector::end_burst(bool terminated) noexcept {
    if (taps_ == 0) return;
    // The C# EmitBurst: a candidate being read in the background goes straight into the live
    // read, keeping its retry and earliest-echo state; otherwise back to the search. The metric
    // ring keeps what the background search wrote, as the C#'s does.
    const bool carry = state_ == State::Background && cand_pending_ && !terminated;
    const bool alt = alt_available_;
    const bool earliest = track_from_earliest_;
    back_to_search();
    if (!carry) return;
    start_live_read();
    alt_available_ = alt;
    track_from_earliest_ = earliest;
}

std::size_t PreambleDetector::process(std::span<const float> audio) noexcept {
    if (taps_ == 0 || state_ == State::Detected) return 0;
    std::size_t used = 0;
    for (const float sample : audio) {
        ++used;
        if (!stream_->push(sample)) continue;
        on_t2();
        if (state_ == State::Detected) break;
    }
    return used;
}

void PreambleDetector::on_t2(bool step) noexcept {
    // The newest sample is in the stream already; the one 576 back is still held (the ring is at
    // least ring_size long), so the running comb energies see the same values as the C#'s.
    const std::int64_t p = stream_->written() - 1;
    const std::size_t parity = static_cast<std::size_t>(p & 1);
    comb_energy_[parity] += static_cast<double>(cmag2(stream_->at(p)));
    if (p >= 576) comb_energy_[parity] -= static_cast<double>(cmag2(stream_->at(p - 576)));
    if (!step) return;

    switch (state_) {
        case State::Searching: search_step(); break;
        case State::ReadingPreamble: try_read_preamble(); break;
        case State::Detected:
        case State::Background: break;  // background_step(), after the burst's own stage
    }
}

// ------------------------------------------------------------------ acquisition

void PreambleDetector::search_step() noexcept {
    const std::int64_t p = stream_->written() - 1;
    if (p < 574) return;

    const std::int64_t s = p - 574;
    const bool shadow = state_ == State::Background;
    if (shadow && s < shadow_from_) {
        // Still inside the locked burst's own preamble: the burst already being read, not a new
        // transmission. Zeroed so the walkback cannot step back into it either.
        metric_ring_[static_cast<std::size_t>(s & 255)] = 0;
        return;
    }
    const double energy = comb_energy_[static_cast<std::size_t>(s & 1)];
    if (energy <= 1e-12) return;

    double best = 0;
    int best_bin = 0;
    for (int b = 0; b < search_bins; ++b) {
        const double m = metric(s, b, energy, nullptr);
        if (m > best) {
            best = m;
            best_bin = b;
        }
    }

    metric_ring_[static_cast<std::size_t>(s & 255)] = best;
    metric_bin_ring_[static_cast<std::size_t>(s & 255)] = static_cast<std::uint8_t>(best_bin);

    if (!shadow && best > stats_.peak_search_metric) stats_.peak_search_metric = best;

    if (best > sync_threshold_ && best > best_metric_) {
        best_metric_ = best;
        best_start_ = s;
        best_bin_ = best_bin;
    }

    if (best_start_ >= 0 && s - best_start_ > 192) accept_peak();
}

double PreambleDetector::metric(std::int64_t s, int bin, double energy, ComplexF* segments) const noexcept {
    const auto& reference = mf_reference_[static_cast<std::size_t>(bin)];
    double sum = 0;
    for (int k = 0; k < 9; ++k) {
        ComplexF c{0, 0};
        int chip = 32 * k;
        for (int i = 0; i < 32; ++i, ++chip) {
            const ComplexF r = stream_->at(s + (2 * chip));
            const ComplexF ref = reference[static_cast<std::size_t>(chip)];
            c = cadd(c, cmul(r, ref));
        }
        if (segments != nullptr) segments[k] = c;
        sum += static_cast<double>(cabs(c));
    }
    return sum / (std::sqrt(static_cast<double>(chips_fixed)) * std::sqrt(energy) + 1e-12);
}

void PreambleDetector::accept_peak() noexcept {
    std::int64_t s = best_start_;
    int bin = best_bin_;
    ++stats_.peaks_accepted;

    // Earliest-arrival selection (see the C#): walk back up to 44 T/2 samples (9 ms, the widest
    // D-LXV spread) and take the first candidate within -4.4 dB of the best.
    const double floor_metric = std::max(sync_threshold_, 0.6 * best_metric_);
    for (std::int64_t candidate = std::max<std::int64_t>(574, s - 44); candidate < s; ++candidate) {
        if (metric_ring_[static_cast<std::size_t>(candidate & 255)] >= floor_metric) {
            s = candidate;
            bin = metric_bin_ring_[static_cast<std::size_t>(candidate & 255)];
            break;
        }
    }

    if (state_ == State::Background) ++stats_.background_candidates;
    // C# issue 8: keep the strongest peak, in case the earliest echo reads badly.
    alt_available_ = s != best_start_;
    alt_start_ = best_start_;
    alt_bin_ = best_bin_;
    accepted_metric_ = best_metric_;
    best_metric_ = 0;
    best_start_ = -1;
    begin_candidate(s, bin);
}

// Times and tunes a candidate whose Fixed section starts at T/2 position s in search bin `bin`
// (the C# BeginCandidate), then reads it: live while searching, in the background during a burst.
void PreambleDetector::begin_candidate(std::int64_t s, int bin) noexcept {
    const double energy = comb_energy_[static_cast<std::size_t>(s & 1)];

    // Sub-sample timing from a parabolic fit through the metric at s-1, s, s+1.
    std::array<ComplexF, 9> segments{};
    const double m0 = metric(s, bin, energy, segments.data());
    const double mm = metric(s - 1, bin, comb_energy_[static_cast<std::size_t>((s - 1) & 1)], nullptr);
    const double mp = metric(s + 1, bin, comb_energy_[static_cast<std::size_t>((s + 1) & 1)], nullptr);
    const double denom = mm - (2 * m0) + mp;
    double delta = std::abs(denom) > 1e-9 ? 0.5 * (mm - mp) / denom : 0;
    delta = std::clamp(delta, -1.0, 1.0);

    // Fine CFO from the phase progression across the nine 32-chip segment correlations
    // (+-37.5 Hz unambiguous, inside the 25 Hz grid residual).
    ComplexF rotation{0, 0};
    for (int k = 0; k < 8; ++k) {
        rotation = cadd(rotation, cmul(segments[static_cast<std::size_t>(k + 1)], cconj(segments[static_cast<std::size_t>(k)])));
    }
    // float * int is a float product in the C#, then the division promotes to double.
    const double fine_hz = static_cast<double>(carg(rotation) * static_cast<float>(tables::symbol_rate)) /
                           (2.0 * std::numbers::pi * 32.0);
    cand_cfo_hz_ = bins_hz[bin] + fine_hz;
    cand_chip0_ = static_cast<double>(s) + delta;
    cand_omega_ = 2.0 * std::numbers::pi * cand_cfo_hz_ / (2.0 * tables::symbol_rate);
    cand_coarse_bin_hz_ = bins_hz[bin];
    cand_fraction_ = delta;

    if (state_ == State::Background) {
        cand_pending_ = true;
        load_candidate();
        return;
    }
    start_live_read();
}

// The reading state from the candidate: its timing and carrier, unity gain, nothing read yet.
void PreambleDetector::load_candidate() noexcept {
    agc_gain_ = 1.0f;
    chip0_ = cand_chip0_;
    omega_ = cand_omega_;
    theta_base_ = 0;
    cfo_hz_ = cand_cfo_hz_;
    coarse_bin_hz_ = cand_coarse_bin_hz_;
    timing_fraction_ = cand_fraction_;
    count_read_ = false;
    tail_pending_ = false;
    votes_ = 0;
}

// The C# StartLiveRead: the candidate becomes the live acquisition.
void PreambleDetector::start_live_read() noexcept {
    cand_pending_ = false;
    load_candidate();
    state_ = State::ReadingPreamble;
    best_metric_ = 0;
    best_start_ = -1;
}

// A candidate was rejected (the C# RejectCandidate). C# issue 8: if the walkback had moved it off
// the strongest peak, read it once more there; otherwise give up on it.
void PreambleDetector::reject_candidate() noexcept {
    if (alt_available_) {
        alt_available_ = false;
        ++stats_.retries;
        track_from_earliest_ = static_cast<double>(alt_start_) - cand_chip0_ >= 4;  // two chips: a separate arrival
        earliest_chip0_ = cand_chip0_;
        earliest_fraction_ = cand_fraction_;
        begin_candidate(alt_start_, alt_bin_);
        return;
    }
    track_from_earliest_ = false;
    if (state_ == State::Background) {
        // Back to the background search. The ring was not written while the candidate was read,
        // so clear it: stale peaks must not steer the walkback.
        cand_pending_ = false;
        metric_ring_.fill(0);
        return;
    }
    back_to_search();
}

// ------------------------------------------------------------------ preamble

PreambleDetector::ComplexF PreambleDetector::interpolate(double pos) const noexcept {
    const auto i0 = static_cast<std::int64_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    const std::int64_t written = stream_->written();
    ComplexF acc{0, 0};
    for (int j = -interp_half + 1; j <= interp_half; ++j) {
        const double u = j - frac;
        double w;
        if (std::abs(u) < 1e-9) {
            w = 1;
        } else {
            w = std::sin(std::numbers::pi * u) / (std::numbers::pi * u) *
                (0.5 + (0.5 * std::cos(std::numbers::pi * u / interp_half)));
        }
        const std::int64_t idx = i0 + j;
        // Look-back bound: a read at or after the oldest sample the ring still holds.
        assert(idx < 0 || idx >= written || written - idx <= static_cast<std::int64_t>(ring_size));
        if (idx >= 0 && idx < written) {
            acc = cadd(acc, cscale(stream_->at(idx), static_cast<float>(w)));
        }
    }
    return acc;
}

// A chip read through the carrier model: interpolated at the chip's T/2 position and derotated.
PreambleDetector::ComplexF PreambleDetector::read_chip(int chip) const noexcept {
    const double half_chips = 2.0 * chip;
    const double pos = chip0_ + half_chips;  // the C# adds a timing correction that is zero until it locks
    const ComplexF value = interpolate(pos);
    const double theta = theta_base_ + (omega_ * (pos - chip0_));
    return cmul(value, from_phase(-static_cast<float>(theta))) * agc_gain_;
}

std::uint8_t PreambleDetector::read_walsh_symbol(int start_chip, const std::array<std::uint8_t, 256>& pn,
                                                 int pn_offset) const noexcept {
    std::array<ComplexF, 4> corr{};
    const auto& w1 = tables::walsh[1];
    const auto& w2 = tables::walsh[2];
    const auto& w3 = tables::walsh[3];
    for (int i = 0; i < 32; ++i) {
        const ComplexF r = cmul(read_chip(start_chip + i), cconj(psk8(pn[static_cast<std::size_t>(pn_offset + i)])));
        corr[0] = cadd(corr[0], r);
        corr[1] = w1[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[1], r) : csub(corr[1], r);
        corr[2] = w2[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[2], r) : csub(corr[2], r);
        corr[3] = w3[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[3], r) : csub(corr[3], r);
    }
    std::uint8_t best_dibit = 0;
    float best = cmag2(corr[0]);
    for (std::uint8_t s = 1; s < 4; ++s) {
        const float m = cmag2(corr[s]);
        if (m > best) {
            best = m;
            best_dibit = s;
        }
    }
    return best_dibit;
}

// Adds the four Walsh hypotheses' correlation magnitudes for one dibit into mags (magnitudes,
// not phasors: super-frames a Rayleigh fade apart are not coherent).
void PreambleDetector::accumulate_walsh_mags(int start_chip, const std::array<std::uint8_t, 256>& pn, int pn_offset,
                                             double* mags) const noexcept {
    std::array<ComplexF, 4> corr{};
    const auto& w1 = tables::walsh[1];
    const auto& w2 = tables::walsh[2];
    const auto& w3 = tables::walsh[3];
    for (int i = 0; i < 32; ++i) {
        const ComplexF r = cmul(read_chip(start_chip + i), cconj(psk8(pn[static_cast<std::size_t>(pn_offset + i)])));
        corr[0] = cadd(corr[0], r);
        corr[1] = w1[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[1], r) : csub(corr[1], r);
        corr[2] = w2[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[2], r) : csub(corr[2], r);
        corr[3] = w3[static_cast<std::size_t>(i & 3)] == 0 ? cadd(corr[3], r) : csub(corr[3], r);
    }
    for (std::size_t s = 0; s < 4; ++s) mags[s] += static_cast<double>(cabs(corr[s]));
}

void PreambleDetector::refine_carrier(int base_chip, const std::uint8_t* known_chips, int chip_count) noexcept {
    const int count = chip_count / 32;
    std::array<ComplexF, 72> groups{};
    for (int j = 0; j < count; ++j) {
        ComplexF c{0, 0};
        for (int i = 0; i < 32; ++i) {
            const int chip = (32 * j) + i;
            c = cadd(c, cmul(read_chip(base_chip + chip), cconj(psk8(known_chips[chip]))));
        }
        groups[static_cast<std::size_t>(j)] = c;
    }

    double slope = 0;
    double intercept = 0;
    if (!estimate_carrier_fit(groups.data(), count, slope, intercept)) return;

    // Anchor the correction at the window midpoint (group j's phase sits at its centre, chip
    // 32 j + 16); the slope converts to rad per T/2 sample by dividing by 64.
    const double mid = (count - 1) / 2.0;
    const double href = 2.0 * (base_chip + 16 + (32 * mid));
    const double delta_theta = intercept + (slope * mid);
    const double delta_omega = slope / 64.0;
    omega_ += delta_omega;
    theta_base_ += delta_theta - (delta_omega * href);
    cfo_hz_ = cfo_hz_ + (slope / 64.0 * 2.0 * tables::symbol_rate / (2.0 * std::numbers::pi));
}

void PreambleDetector::try_read_preamble() noexcept {
    if (tail_pending_) {
        finish_tail();
        return;
    }
    switch (read_preamble()) {
        case Read::NotReady: break;
        case Read::Rejected: reject_candidate(); break;
        case Read::Locked: enter_lock(); break;
    }
}

// Reads the downcount and waveform ID at the current timing and carrier and applies every
// acquisition gate (the C# ReadPreamble). On Locked the read_ fields hold what it announced.
PreambleDetector::Read PreambleDetector::read_preamble() noexcept {
    const auto need = [this](int super_frames) {
        return static_cast<std::int64_t>(std::ceil(chip0_ + (2.0 * chips_superframe * super_frames))) + (2 * interp_half) + 2;
    };
    // The whole matched super-frame plus interpolation margin.
    if (stream_->written() < need(1)) return Read::NotReady;

    if (!count_read_) {
        std::array<std::uint8_t, 4> count_dibits{};
        for (int j = 0; j < 4; ++j) {
            count_dibits[static_cast<std::size_t>(j)] = read_walsh_symbol(chips_fixed + (32 * j), tables::cnt_pn, 32 * j);
        }
        const auto single = decode_count(count_dibits);
        if (!single) {
            ++stats_.rejected_count;
            return Read::Rejected;
        }
        // The WID section repeats identically in every remaining super-frame, all of which
        // arrive before the data, so it and the downcount are soft-combined across them at no
        // latency cost.
        votes_ = std::min(*single + 1, 5);
        count_read_ = true;
    }

    const int votes = votes_;
    if (stream_->written() < need(votes)) return Read::NotReady;  // wait for the vote span, still entirely pre-data

    std::array<double, 5 * 4> wid_mags{};
    for (int v = 0; v < votes; ++v) {
        for (int j = 0; j < 5; ++j) {
            accumulate_walsh_mags((v * chips_superframe) + chips_fixed + 128 + (32 * j), tables::wid_pn, 32 * j,
                                  &wid_mags[static_cast<std::size_t>(j * 4)]);
        }
    }

    std::array<std::uint8_t, 5> wid_dibits{};
    double margin_sum = 0;
    for (int j = 0; j < 5; ++j) {
        const double* mags = &wid_mags[static_cast<std::size_t>(j * 4)];
        std::uint8_t best_dibit = 0;
        for (std::uint8_t s = 1; s < 4; ++s) {
            if (mags[s] > mags[best_dibit]) best_dibit = s;
        }
        double second = 0;
        for (std::uint8_t s = 0; s < 4; ++s) {
            if (s != best_dibit && mags[s] > second) second = mags[s];
        }
        margin_sum += (mags[best_dibit] - second) / (mags[best_dibit] + 1e-9);
        wid_dibits[static_cast<std::size_t>(j)] = best_dibit;
    }

    // Vote confidence gate: a wrong acquisition turns every Walsh correlation to mush, and an
    // argmax over summed noise beats the weak checksum about 1 in 8.
    const double vote_margin = margin_sum / 5;
    if (vote_margin < 0.20) {
        ++stats_.rejected_margin;
        return Read::Rejected;
    }

    // Joint downcount vote over the same span: the field decrements per super-frame, so
    // candidate c at vote frame v expects the count word of c - v.
    std::array<double, 5 * 4 * 4> cnt_mags{};
    for (int v = 0; v < votes; ++v) {
        for (int j = 0; j < 4; ++j) {
            accumulate_walsh_mags((v * chips_superframe) + chips_fixed + (32 * j), tables::cnt_pn, 32 * j,
                                  &cnt_mags[static_cast<std::size_t>(((v * 4) + j) * 4)]);
        }
    }

    double best_score = -1;
    double second_score = -1;
    int joint_count = -1;
    for (int c = votes - 1; c < 32; ++c) {
        double score = 0;
        for (int v = 0; v < votes; ++v) {
            const auto expected = encode_count(c - v);
            for (int j = 0; j < 4; ++j) {
                score += cnt_mags[static_cast<std::size_t>((((v * 4) + j) * 4) + (*expected)[static_cast<std::size_t>(j)])];
            }
        }
        if (score > best_score) {
            second_score = best_score;
            best_score = score;
            joint_count = c;
        } else if (score > second_score) {
            second_score = score;
        }
    }

    const double count_margin = (best_score - second_score) / (best_score + 1e-9);
    if (count_margin < 0.10) {
        ++stats_.rejected_count_vote;
        return Read::Rejected;
    }
    const int count = joint_count;

    // A corrupted waveform ID can pass its checksum and still name a pair no table defines.
    const auto wid = decode_wid(wid_dibits);
    const bool supported = wid && ((wid->wn >= 0 && wid->wn <= 8) || wid->wn == 13) && has_interleaver_3k(wid->wn, wid->interleaver);
    if (!supported) {
        ++stats_.rejected_wid;
        return Read::Rejected;
    }
    read_count_ = count;
    read_wn_ = wid->wn;
    read_il_ = wid->interleaver;
    read_k_ = wid->constraint_length;
    return Read::Locked;
}

// The lock (the C# EnterTracking up to its carrier fit), then the tail fit.
void PreambleDetector::enter_lock() noexcept {
    const int count = read_count_;
    if (track_from_earliest_) {
        // C# issue 8: the strongest peak read the preamble; the equaliser starts on the earliest
        // echo. Both lie inside one super-frame, so the count still holds, and the carrier fit
        // below re-anchors the phase there.
        track_from_earliest_ = false;
        chip0_ = earliest_chip0_;
        theta_base_ = 0;
        timing_fraction_ = earliest_fraction_;
    }
    alt_available_ = false;

    // Known chips of the matched super-frame (downcount `count`), for the carrier fit.
    std::array<std::uint8_t, chips_superframe> known{};
    superframe_chips(count, read_wn_, read_il_, read_k_, known.data());
    refine_carrier(0, known.data(), chips_superframe);

    const std::int64_t data_start_chip = static_cast<std::int64_t>(chips_superframe) * (count + 1);
    PreambleDetection e;
    e.waveform = read_wn_;
    e.interleaver = read_il_;
    e.constraint_length = read_k_;
    e.superframes_after = count;
    e.chip0_t2 = chip0_;
    e.data_start_t2 = chip0_ + (2.0 * static_cast<double>(data_start_chip));
    e.data_start_sample = (2.0 * e.data_start_t2) + 1.0 - (static_cast<double>(taps_ - 1) / 2.0);
    e.timing_fraction_t2 = timing_fraction_;
    e.locked_at_sample = stream_->used();
    e.cfo_hz_first_fit = cfo_hz_;
    e.carrier_omega_first_fit = omega_;
    e.carrier_theta_base_first_fit = theta_base_;
    e.coarse_bin_hz = coarse_bin_hz_;
    e.peak_metric = accepted_metric_;
    event_ = e;
    ++stats_.locks;

    // C# issue 10: from the burst's data start the search runs on in the background.
    shadow_from_ = static_cast<std::int64_t>(std::ceil(chip0_ + (2.0 * static_cast<double>(data_start_chip))));
    cand_pending_ = false;
    best_metric_ = 0;
    best_start_ = -1;
    metric_ring_.fill(0);
    tail_pending_ = true;
    finish_tail();
}

// Writes the 576 known chips of the super-frame that carries downcount `count`. The Fixed section
// is always the 9 symbol form, so they come from a preamble of two or more super-frames.
void PreambleDetector::superframe_chips(int count, int wn, InterleaverKind il, int k, std::uint8_t* out) noexcept {
    const int m = std::max(count + 1, 2);
    const auto preamble = Preamble::make(0, m, wn, il, k);
    const std::size_t base = static_cast<std::size_t>(m - 1 - count) * chips_superframe;
    for (std::size_t i = 0; i < chips_superframe; ++i) out[i] = preamble->chip(base + i);
}

double PreambleDetector::estimate_input_level(std::int64_t data_start_chip) const noexcept {
    std::array<std::uint8_t, chips_superframe> fixed{};
    superframe_chips(0, 0, InterleaverKind::Short, 7, fixed.data());  // the Fixed section is the first 288
    const int available = static_cast<int>(std::clamp<std::int64_t>(data_start_chip / chips_superframe, 1, 8));
    double sum = 0;
    int groups = 0;
    for (int sf = 1; sf <= available; ++sf) {
        const int base = static_cast<int>(data_start_chip) - (sf * chips_superframe);
        for (int g = 0; g < chips_fixed / 32; ++g) {
            ComplexF c{0, 0};
            for (int i = 0; i < 32; ++i) {
                const int chip = (g * 32) + i;
                c = cadd(c, cmul(read_chip(base + chip), cconj(psk8(fixed[static_cast<std::size_t>(chip)]))));
            }
            sum += static_cast<double>(cabs(c)) / 32.0;
            ++groups;
        }
    }
    return groups > 0 ? sum / groups : 0;
}

// The C# re-fits the carrier over the last preamble super-frames (up to four, about a second)
// just before the data, because the matched super-frame can be many seconds back and the phase
// extrapolated from it stale (TrackWalsh and InitializeDfe both do it first). It needs the
// samples up to the data start, so the event waits for them: at once when the preamble is short,
// up to the data start when the downcount is above four.
void PreambleDetector::finish_tail() noexcept {
    const int count = event_.superframes_after;
    const std::int64_t data_start_chip = static_cast<std::int64_t>(chips_superframe) * (count + 1);
    const double last = chip0_ + (2.0 * static_cast<double>(data_start_chip + 2));
    if (stream_->written() <= static_cast<std::int64_t>(std::ceil(last)) + interp_half + 1) return;

    // The input level, read with the first-fit carrier; the equaliser path then reads everything
    // through the gain it implies (the C# sets it before the re-fit).
    event_.input_level = estimate_input_level(data_start_chip);
    const float gain = level_gain(event_.waveform, event_.input_level);
    if (event_.waveform != 0) {
        event_.agc_gain = gain;
        agc_gain_ = gain;
    }

    const int tail = std::clamp(count + 1, 1, 4);
    std::array<std::uint8_t, 4 * chips_superframe> known{};
    for (int i = 0; i < tail; ++i) {
        superframe_chips(tail - 1 - i, event_.waveform, event_.interleaver, event_.constraint_length,
                         &known[static_cast<std::size_t>(i * chips_superframe)]);
    }
    refine_carrier(static_cast<int>(data_start_chip) - (tail * chips_superframe), known.data(), tail * chips_superframe);

    agc_gain_ = 1.0f;
    event_.cfo_hz = cfo_hz_;
    event_.carrier_omega = omega_;
    event_.carrier_theta_base = theta_base_;
    event_.detected_at_sample = stream_->used();
    tail_pending_ = false;
    ++stats_.detections;
    state_ = State::Detected;
}

}  // namespace m0lte::ms110d
