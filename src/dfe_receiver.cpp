// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "dfe_receiver.hpp"

#include "dfe_common.hpp"

#include "preamble.hpp"
#include "preamble_detector.hpp"
#include "tables.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

using dsp::ComplexF;

constexpr int interp_half = 4;  // 8 tap interpolator
constexpr int chips_fixed = 288;
constexpr int chips_superframe = 576;
constexpr double pi = std::numbers::pi;

// The fading detector (the C# FadeExcursionRatio, FadeEnterWindowFrames, FadeExitFrames).
constexpr float fade_excursion_ratio = 3.5f;
constexpr int fade_enter_window_frames = 24;
constexpr int fade_exit_frames = 32;
constexpr int frames_since_excursion_idle = 2147483647 / 2;  // int.MaxValue / 2

// Signal-absent release (issue 553 in the C#).
constexpr double presence_window_seconds = 16.0;
constexpr double presence_sigmas = 4.0;
constexpr double presence_background_seconds = 2.0;
constexpr double presence_spectrum_seconds = 4.0;
constexpr int presence_max_carriers = 4;
constexpr double presence_carrier_over_noise = 1.5;
constexpr double presence_carrier_over_signal = 4.0;

using dfe_detail::cf;
using dfe_detail::clamp;
using dfe_detail::eq;
using dfe_detail::psk8;
using dfe_detail::symbol_to_tribit8;

// Squared decision-confidence radius per modulation (the C# DdGateRadius).
float dd_gate_radius(Modulation m) noexcept {
    switch (m) {
        case Modulation::Qam16: return 0.0225f;
        case Modulation::Psk8: return 0.16f;
        default: return 0.4f;
    }
}

ComplexF nearest_point(ComplexF y, const ComplexF* constellation, int points) noexcept {
    float best = 3.40282347e+38f;  // float.MaxValue
    ComplexF result = constellation[0];
    for (int s = 0; s < points; ++s) {
        const float d = dsp::mag2(y - constellation[s]);
        if (d < best) {
            best = d;
            result = constellation[s];
        }
    }
    return result;
}

// The constellations as C#-style complex values (built at compile time, so no static
// initialisation guard is needed on a bare-metal target).
struct Constellations {
    std::array<ComplexF, 8> psk8{};
    std::array<ComplexF, 16> qam16{};
};

constexpr Constellations k_constellations = [] {
    Constellations k;
    for (std::size_t i = 0; i < 8; ++i) k.psk8[i] = {tables::psk8[i].real(), tables::psk8[i].imag()};
    for (std::size_t i = 0; i < 16; ++i) k.qam16[i] = {tables::qam16[i].real(), tables::qam16[i].imag()};
    return k;
}();

constexpr const Constellations& constellations() noexcept { return k_constellations; }

// The C# Slice: the nearest constellation point to a descrambled symbol.
ComplexF slice(ComplexF d, Modulation m) noexcept {
    if (m == Modulation::Bpsk) return d.re >= 0 ? ComplexF{1, 0} : ComplexF{-1, 0};
    if (m == Modulation::Psk8) return nearest_point(d, constellations().psk8.data(), 8);
    if (m == Modulation::Qam16) return nearest_point(d, constellations().qam16.data(), 16);
    // QPSK points sit on the axes (Table D-IV -> 8PSK symbols 0, 2, 4, 6).
    return std::abs(d.re) >= std::abs(d.im) ? ComplexF{d.re >= 0 ? 1.0f : -1.0f, 0}
                                            : ComplexF{0, d.im >= 0 ? 1.0f : -1.0f};
}

// The C# PresenceLags: half a probe base period either side of the cursor.
int presence_lags(int k) noexcept {
    const tables::ProbeSpec* spec = tables::find_probe_spec(k);
    const int base = spec == nullptr ? 1 : static_cast<int>(spec->base.size());
    return (2 * ((base - 1) / 2)) + 1;
}

double presence_bin_frequency(int j, int bins) noexcept { return (2.0 * pi * j / bins) - pi; }

double expected_median_of_unit_exponentials(int n) noexcept {
    double sum = 0;
    for (int i = (n + 1) / 2; i <= n; ++i) sum += 1.0 / i;
    return sum;
}

// The C# TapTrajectory: linear interpolation of the de-rotated taps with the anchors' common
// rotation phi applied as a phase ramp.
void tap_trajectory(const EqSample* start, const EqSample* end_derotated, double phi, float f, EqSample* taps,
                    int n) noexcept {
    const ComplexF ramp = dsp::from_phase(static_cast<float>(phi * static_cast<double>(f)));
    const float g = 1.0f - f;
    for (int i = 0; i < n; ++i) taps[i] = eq(((cf(start[i]) * g) + (cf(end_derotated[i]) * f)) * ramp);
}

// The C# FrameFraction.
float frame_fraction(const Mode& mode, int u) noexcept {
    return (static_cast<float>(u) + (0.5f * static_cast<float>(mode.k))) / static_cast<float>(mode.u + mode.k);
}

std::size_t next_pow2(std::size_t n) noexcept {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

struct Sizes {
    std::size_t symbols = 0;  // data symbols in the longest block
    std::size_t ring = 0;
    std::size_t dfe = 0;
    std::size_t size_bits = 0;
    std::size_t input_bits = 0;
    std::size_t dft = 0;  // doubles in each of the two presence DFT tables
};

Sizes sizes_for(InterleaverKind max_kind) noexcept {
    Sizes s;
    std::size_t ring_need = 0;
    for (const Mode& m : modes_3k()) {
        if (m.wn == 0) continue;
        const EqParams p = eq_params_for_probe_length(m.k);
        s.dfe = std::max(s.dfe, Dfe::storage_bytes(p.ff_taps, p.fb_taps));
        const int lags = presence_lags(m.k);
        s.dft = std::max(s.dft, static_cast<std::size_t>(4 * lags * lags));
        for (int kind = 0; kind <= static_cast<int>(max_kind); ++kind) {
            const auto il = interleaver_3k(m.wn, static_cast<InterleaverKind>(kind));
            if (!il) continue;
            s.size_bits = std::max(s.size_bits, static_cast<std::size_t>(il->size_bits));
            s.input_bits = std::max(s.input_bits, static_cast<std::size_t>(il->input_bits));
            s.symbols = std::max(s.symbols, static_cast<std::size_t>(il->frames) * static_cast<std::size_t>(m.u));
            // A whole block (the turbo pass re-reads it at the block's end), the probe after it,
            // the last preamble super-frame (the initial solve) and the reads around them.
            const std::size_t symbols = static_cast<std::size_t>(il->frames) * static_cast<std::size_t>(m.u + m.k) +
                                        static_cast<std::size_t>(2 * m.k + chips_superframe);
            ring_need = std::max(ring_need, (2 * symbols) + 256);
        }
    }
    s.ring = next_pow2(std::max<std::size_t>(ring_need, 4096));
    return s;
}

// Carves aligned pieces out of the caller's block, counting when `base` is null.
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

// Where every piece of the caller's block goes. carve_all lays them out in one order, for sizing
// (a null base) and for carving.
struct Layout {
    ComplexF* ring;
    std::uint8_t* dfe;
    float *block_llrs, *deinterleaved, *mother;
    std::uint8_t* info;
    double *dft_re, *dft_im;
    float *first_pass_llrs, *turbo_llrs, *soft_punctured, *soft_mother, *soft_mother_post, *soft_wire_llrs,
        *soft_wire_ext, *soft_var;
    ComplexF *soft_expected, *turbo_expected;
    std::uint8_t *first_info, *prev_info, *mfb_info, *fetched, *fetched2, *coded, *punctured_bits, *mfb;
};

Layout carve_all(Carver& c, const Sizes& s, std::size_t mfb_bytes) noexcept {
    Layout l{};
    l.ring = c.take<ComplexF>(s.ring);
    l.dfe = c.take<std::uint8_t>(s.dfe);
    l.block_llrs = c.take<float>(s.size_bits);
    l.deinterleaved = c.take<float>(s.size_bits);
    l.mother = c.take<float>(2 * s.input_bits);
    l.info = c.take<std::uint8_t>(s.input_bits);
    l.dft_re = c.take<double>(s.dft);
    l.dft_im = c.take<double>(s.dft);
    l.first_pass_llrs = c.take<float>(s.size_bits);
    l.turbo_llrs = c.take<float>(s.size_bits);
    l.soft_punctured = c.take<float>(s.size_bits);
    l.soft_mother = c.take<float>(2 * s.input_bits);
    l.soft_mother_post = c.take<float>(2 * s.input_bits);
    l.soft_wire_llrs = c.take<float>(s.size_bits);
    l.soft_wire_ext = c.take<float>(s.size_bits);
    l.soft_var = c.take<float>(s.symbols);
    l.soft_expected = c.take<ComplexF>(s.symbols);
    l.turbo_expected = c.take<ComplexF>(s.symbols);
    l.first_info = c.take<std::uint8_t>(s.input_bits);
    l.prev_info = c.take<std::uint8_t>(s.input_bits);
    l.mfb_info = c.take<std::uint8_t>(s.input_bits);
    l.fetched = c.take<std::uint8_t>(s.size_bits);
    l.fetched2 = c.take<std::uint8_t>(s.size_bits);
    l.coded = c.take<std::uint8_t>(2 * s.input_bits);
    l.punctured_bits = c.take<std::uint8_t>(s.size_bits);
    l.mfb = c.take<std::uint8_t>(mfb_bytes);
    return l;
}

}  // namespace

DfeStart dfe_start_from_detection(const PreambleDetection& d) noexcept {
    DfeStart s;
    s.waveform = d.waveform;
    s.interleaver = d.interleaver;
    s.constraint_length = d.constraint_length;
    s.chip0 = d.chip0_t2;
    s.carrier_omega = d.carrier_omega;
    s.carrier_theta_base = d.carrier_theta_base;
    s.agc_gain = d.agc_gain;
    s.data_start_chip = static_cast<std::int64_t>(chips_superframe) * (d.superframes_after + 1);
    return s;
}

std::size_t DfeReceiver::ring_size_for(InterleaverKind max_interleaver) noexcept {
    return sizes_for(max_interleaver).ring;
}

std::size_t DfeReceiver::max_input_bits_for(InterleaverKind max_interleaver) noexcept {
    return sizes_for(max_interleaver).input_bits;
}

std::size_t DfeReceiver::storage_bytes(const DfeReceiverConfig& config) noexcept {
    if (static_cast<int>(config.max_interleaver) < 0 || static_cast<int>(config.max_interleaver) > 3) return 0;
    Carver c{nullptr};
    carve_all(c, sizes_for(config.max_interleaver), MfbBlockDecoder::storage_bytes(config.max_interleaver));
    return c.at + alignof(double);  // room to align the start
}

bool DfeReceiver::configure(const DfeReceiverConfig& config, std::span<std::uint8_t> storage,
                            const DfeDecoders& decoders) noexcept {
    ring_ = nullptr;
    active_ = false;
    const std::size_t need = storage_bytes(config);
    const DfeDecoders& d = decoders;
    if (need == 0 || storage.size() < need || d.viterbi_k7 == nullptr || d.viterbi_k9 == nullptr ||
        d.siso_k7 == nullptr || d.siso_k9 == nullptr || d.chain == nullptr || !d.viterbi_k7->valid() ||
        !d.viterbi_k9->valid() || !d.siso_k7->valid() || !d.siso_k9->valid()) {
        return false;
    }
    if (config.max_input_data_blocks < 0) return false;
    if (!front_end_.configure(config.roll_off)) return false;
    const Sizes s = sizes_for(config.max_interleaver);
    void* where = storage.data();
    std::size_t space = storage.size();
    if (std::align(alignof(double), need - alignof(double), where, space) == nullptr) return false;
    Carver c{static_cast<std::uint8_t*>(where)};
    const std::size_t mfb_bytes = MfbBlockDecoder::storage_bytes(config.max_interleaver);
    const Layout l = carve_all(c, s, mfb_bytes);
    if (!mfb_.configure({l.mfb, mfb_bytes}, config.max_interleaver)) return false;
    dfe_storage_ = {l.dfe, s.dfe};
    block_llrs_ = l.block_llrs;
    deinterleaved_ = l.deinterleaved;
    mother_ = l.mother;
    info_ = l.info;
    presence_dft_re_ = l.dft_re;
    presence_dft_im_ = l.dft_im;
    first_pass_llrs_ = l.first_pass_llrs;
    turbo_llrs_ = l.turbo_llrs;
    soft_punctured_ = l.soft_punctured;
    soft_mother_ = l.soft_mother;
    soft_mother_post_ = l.soft_mother_post;
    soft_wire_llrs_ = l.soft_wire_llrs;
    soft_wire_ext_ = l.soft_wire_ext;
    soft_var_ = l.soft_var;
    soft_expected_ = l.soft_expected;
    turbo_expected_ = l.turbo_expected;
    first_info_ = l.first_info;
    prev_info_ = l.prev_info;
    mfb_info_ = l.mfb_info;
    fetched_ = l.fetched;
    fetched2_ = l.fetched2;
    coded_ = l.coded;
    punctured_bits_ = l.punctured_bits;
    ring_size_ = s.ring;
    max_size_bits_ = s.size_bits;
    max_input_bits_ = s.input_bits;
    max_block_symbols_ = s.symbols;
    config_ = config;
    decoders_ = decoders;
    ring_ = l.ring;
    reset();
    return true;
}

void DfeReceiver::reset() noexcept {
    if (ring_ == nullptr) return;
    front_end_.reset();
    std::fill(ring_, ring_ + ring_size_, ComplexF{});
    written_ = 0;
    active_ = false;
    tracking_initialized_ = false;
    pending_block_ = false;
    pending_end_ = false;
    stats_ = {};
}

// ------------------------------------------------------------------ the burst

bool DfeReceiver::start(const DfeStart& s) noexcept {
    if (ring_ == nullptr || active_ || pending_block_ || pending_end_) return false;
    const auto mode = mode_3k(s.waveform);
    if (!mode || mode->wn == 0 || static_cast<int>(s.interleaver) > static_cast<int>(config_.max_interleaver)) return false;
    const auto il = interleaver_3k(s.waveform, s.interleaver);
    const auto long_il = interleaver_3k(s.waveform, InterleaverKind::Long);
    if (!il || !long_il || static_cast<std::size_t>(il->size_bits) > max_size_bits_ ||
        static_cast<std::size_t>(il->input_bits) > max_input_bits_ || il->frames > max_block_frames) {
        return false;
    }
    if (s.constraint_length != 7 && s.constraint_length != 9) return false;
    const fec::ConvolutionalCode& code = s.constraint_length == 9 ? fec::k9_561_753 : fec::k7_133_171;
    const PunctureSpec* puncture = find_puncture(code, static_cast<unsigned>(mode->code_rate.numerator),
                                                 static_cast<unsigned>(mode->code_rate.denominator));
    const tables::ProbeSpec* probe_spec = tables::find_probe_spec(mode->k);
    if (puncture == nullptr || probe_spec == nullptr || mode->k > max_k || mode->u > max_u) return false;
    const auto punctured = punctured_length(*puncture, static_cast<std::size_t>(il->input_bits));
    if (!punctured || *punctured != static_cast<std::size_t>(il->size_bits)) return false;

    mode_ = *mode;
    interleaver_kind_ = s.interleaver;
    constraint_length_ = s.constraint_length;
    il_ = *il;
    interleaver_ = Interleaver(static_cast<std::uint32_t>(il->size_bits), static_cast<std::uint32_t>(il->increment));
    puncture_ = puncture;
    viterbi_ = s.constraint_length == 9 ? decoders_.viterbi_k9 : decoders_.viterbi_k7;
    siso_ = s.constraint_length == 9 ? decoders_.siso_k9 : decoders_.siso_k7;
    code_ = &code;
    mfb_ready_ = false;
    frozen_relock_active_ = false;
    for (int i = 0; i < mode_.k; ++i) {
        probe_plain_[static_cast<std::size_t>(i)] = cf(tables::probe_symbol(*probe_spec, i, false));
        probe_shifted_[static_cast<std::size_t>(i)] = cf(tables::probe_symbol(*probe_spec, i, true));
    }

    // The lock (the end of the C# TryReadPreamble) and the burst state EndBurst leaves behind.
    chip0_ = s.chip0;
    tau_ = 0;  // AcceptPeak zeroes it and nothing moves it before tracking
    omega_ = s.carrier_omega;
    theta_base_ = s.carrier_theta_base;
    agc_gain_ = s.agc_gain;
    data_start_chip_ = s.data_start_chip;
    block_llr_count_ = 0;
    block_index_ = 0;
    pattern_.configure(il_.frames, long_il->frames);
    burst_bits_ = 0;
    eom_tail_len_ = 0;
    block_frames_ = 0;
    fade_floor_ = 0;
    fade_floor_seeded_ = false;
    frames_since_excursion_ = frames_since_excursion_idle;
    fading_ = false;
    fading_latched_ = false;
    tracking_initialized_ = false;
    active_ = true;
    return true;
}

std::size_t DfeReceiver::push_audio(std::span<const float> audio) noexcept {
    if (ring_ == nullptr || pending_block_ || pending_end_) return 0;
    std::size_t used = 0;
    for (const float sample : audio) {
        ++used;
        ComplexF t2;
        if (!front_end_.push(sample, t2)) continue;
        push_t2(t2);
        if (pending_block_ || pending_end_) break;
    }
    return used;
}

DfeEvent DfeReceiver::event() const noexcept {
    if (pending_block_) return DfeEvent::BlockDecoded;
    if (pending_end_) return DfeEvent::BurstEnded;
    return DfeEvent::None;
}

void DfeReceiver::take_event() noexcept {
    if (pending_block_) {
        pending_block_ = false;
    } else {
        pending_end_ = false;
    }
}

void DfeReceiver::push_t2(ComplexF sample) noexcept {
    ring_[static_cast<std::size_t>(written_) & (ring_size_ - 1)] = sample;
    ++written_;
    if (active_) try_track();
}

void DfeReceiver::try_track() noexcept {
    if (!tracking_initialized_) {
        if (!have_samples_for_chip(static_cast<double>(data_start_chip_ + mode_.k + 4))) return;
        initialize_dfe();
        tracking_initialized_ = true;
    }
    while (active_ && have_samples_for_chip(static_cast<double>(frame_chip_ + mode_.u + mode_.k + 4))) {
        process_frame();
    }
}

// ------------------------------------------------------------------ sample access

bool DfeReceiver::have_samples_for_chip(double chip) const noexcept {
    const double pos = chip0_ + (2.0 * chip) + tau_;  // the C# PositionOfChip
    return written_ > static_cast<std::int64_t>(std::ceil(pos)) + interp_half + 1;
}

ComplexF DfeReceiver::interpolate(double pos) const noexcept {
    const auto i0 = static_cast<std::int64_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    ComplexF acc{0, 0};
    for (int j = -interp_half + 1; j <= interp_half; ++j) {
        const double u = j - frac;
        double w;
        if (std::abs(u) < 1e-9) {
            w = 1;
        } else {
            w = std::sin(pi * u) / (pi * u) * (0.5 + (0.5 * std::cos(pi * u / interp_half)));
        }
        const std::int64_t idx = i0 + j;
        // Look-back bound: every read is of a sample the ring still holds.
        assert(idx < 0 || idx >= written_ || written_ - idx <= static_cast<std::int64_t>(ring_size_));
        if (idx >= 0 && idx < written_) {
            acc = acc + (ring_[static_cast<std::size_t>(idx) & (ring_size_ - 1)] * static_cast<float>(w));
        }
    }
    return acc;
}

// The C# ReadT2: interpolated at the timing-corrected position, derotated by the carrier model,
// then through the level gain.
ComplexF DfeReceiver::read_t2(double half_chips) const noexcept {
    const double pos = chip0_ + half_chips + tau_;
    const ComplexF value = interpolate(pos);
    const double theta = theta_base_ + (omega_ * (pos - chip0_));
    return (value * dsp::from_phase(-static_cast<float>(theta))) * agc_gain_;
}

void DfeReceiver::fill_window(double symbol_chip, EqSample* window) const noexcept {
    const double h = 2.0 * symbol_chip;
    for (int i = 0; i < nff_; ++i) window[i] = eq(read_t2(h + ff_lead_ - i));
}

// ------------------------------------------------------------------ equaliser start

void DfeReceiver::initialize_dfe() noexcept {
    // The level gain and the carrier re-fit over the last super-frames happened in the detector,
    // as the C# does them first here.
    omega_acquired_ = omega_;

    const EqParams p = eq_params_for_probe_length(mode_.k);
    nff_ = p.ff_taps;
    nfb_ = p.fb_taps;
    ff_lead_ = p.lead;
    init_ridge_ = p.init_ridge;
    track_ridge_ = p.track_ridge;
    dfe_.configure(nff_, nfb_, dfe_storage_);

    // Known symbols for chips [dataStart - 576, dataStart + K): the final super-frame (count 0)
    // and the probe that ends the preamble.
    const int k = mode_.k;
    const auto last = Preamble::make(0, 2, mode_.wn, interleaver_kind_, constraint_length_);
    for (int i = 0; i < chips_superframe; ++i) {
        known_[static_cast<std::size_t>(i)] = psk8(last->chip(static_cast<std::size_t>(chips_superframe + i)));
    }
    for (int i = 0; i < k; ++i) known_[static_cast<std::size_t>(chips_superframe + i)] = probe_plain_[static_cast<std::size_t>(i)];

    // Regularised least-squares solve over the last half super-frame and the probe.
    const std::int64_t base_chip = data_start_chip_ - chips_superframe;
    dfe_.begin_training();
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> past{past_.data(), static_cast<std::size_t>(nfb_)};
    for (int n = chips_fixed; n < chips_superframe + k; ++n) {
        fill_window(static_cast<double>(base_chip + n), window_.data());
        for (int j = 0; j < nfb_; ++j) past_[static_cast<std::size_t>(j)] = eq(known_[static_cast<std::size_t>(n - 1 - j)]);
        dfe_.add_training_row(window, past, eq(known_[static_cast<std::size_t>(n)]));
    }

    // RLS forgetting tied to the frame: lambda = 1 - ln 10 / U (the C# default policy).
    dfe_.begin_rls(static_cast<float>(1.0 - (std::log(10.0) / mode_.u)), 1.0f);
    dfe_.seed_rls_from_training(init_ridge_, 1.0f, 0.0f);
    dfe_.solve_training(init_ridge_, false, 0.0f);
    dfe_.begin_training();

    // Seed the decision history with the probe tail and measure the training statistics.
    for (int j = 0; j < nfb_; ++j) {
        decisions_[static_cast<std::size_t>(j)] = eq(known_[static_cast<std::size_t>(chips_superframe + k - 1 - j)]);
    }
    double mse = 0;
    double energy = 0;
    ComplexF gain{0, 0};
    for (int i = 0; i < k; ++i) {
        const int n = chips_superframe + i;
        fill_window(static_cast<double>(base_chip + n), window_.data());
        for (int j = 0; j < nfb_; ++j) past_[static_cast<std::size_t>(j)] = eq(known_[static_cast<std::size_t>(n - 1 - j)]);
        const ComplexF y = cf(dfe_.equalise(window, past));
        const ComplexF want = known_[static_cast<std::size_t>(n)];
        gain = gain + (y * dsp::conj(want));
        mse += static_cast<double>(dsp::mag2(y - want));
        energy += static_cast<double>(dsp::mag2(cf(window_[static_cast<std::size_t>(ff_lead_)])));
    }
    probe_mse_ = mse / k;
    probe_gain_ref_ = static_cast<double>(dsp::abs(gain) / static_cast<float>(k));
    probe_energy_ref_ = energy / k;
    bad_probes_ = 0;
    collapsed_probes_ = 0;
    collapse_armed_ = true;
    start_presence_window();
    frame_chip_ = data_start_chip_ + k;
    frame_in_block_ = 0;
}

// ------------------------------------------------------------------ probe pattern

// The C# ScoreProbePatterns: each hypothesis for the probe after frame g is run through the
// equaliser with its own probe as the feedback history and compared with its own probe. Returns
// (mse_current - mse_standard) / sum, positive when the standard pattern fits better, 0 when
// neither fits. Read-only on the equaliser.
double DfeReceiver::score_probe_patterns(std::int64_t probe_chip, std::int64_t frame_number) noexcept {
    const ComplexF* current = probe(pattern_.current(frame_number));
    const ComplexF* standard = probe(pattern_.standard(frame_number));
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> past_current{past_.data(), static_cast<std::size_t>(nfb_)};
    const std::span<const EqSample> past_standard{past2_.data(), static_cast<std::size_t>(nfb_)};
    double mse_current = 0;
    double mse_standard = 0;
    for (int i = nfb_; i < mode_.k; ++i) {
        fill_window(static_cast<double>(probe_chip + i), window_.data());
        for (int j = 0; j < nfb_; ++j) {
            past_[static_cast<std::size_t>(j)] = eq(current[i - 1 - j]);
            past2_[static_cast<std::size_t>(j)] = eq(standard[i - 1 - j]);
        }
        mse_current += static_cast<double>(dsp::mag2(cf(dfe_.equalise(window, past_current)) - current[i]));
        mse_standard += static_cast<double>(dsp::mag2(cf(dfe_.equalise(window, past_standard)) - standard[i]));
    }

    // A faded or noisy probe fits neither hypothesis and must not tip the sum.
    const double rows = mode_.k - nfb_;
    if (std::min(mse_current, mse_standard) > 0.5 * rows) return 0;
    const double total = mse_current + mse_standard;
    return total > 0 ? (mse_current - mse_standard) / total : 0;
}

// ------------------------------------------------------------------ one frame

void DfeReceiver::process_frame() noexcept {
    const Mode& mode = mode_;
    const int k = mode.k;
    const int fb = nfb_;
    const int n_taps = nff_ + nfb_;
    const std::span<const EqSample> window{window_.data(), static_cast<std::size_t>(nff_)};
    const std::span<const EqSample> probe_past{past_.data(), static_cast<std::size_t>(fb)};
    const std::span<const EqSample> decisions{decisions_.data(), static_cast<std::size_t>(fb)};
    const std::span<EqSample> start_taps{start_taps_.data(), static_cast<std::size_t>(n_taps)};
    const std::span<EqSample> end_taps{end_taps_.data(), static_cast<std::size_t>(n_taps)};

    // Probe-directed batch least squares: the probe's rows go into the Gram with authoritative
    // weight; the ridge (anchored to the current taps) handles its rank deficiency.
    const std::int64_t probe_chip = frame_chip_ + mode.u;
    const std::int64_t frame_number = (static_cast<std::int64_t>(block_index_) * il_.frames) + frame_in_block_;
    if (pattern_.needs_evidence(frame_number)) {
        // The two shifted-probe conventions disagree here: let the received probe pick.
        pattern_.add_evidence(score_probe_patterns(probe_chip, frame_number));
    }
    const bool shifted = pattern_.boundary(frame_number);
    const ComplexF* pr = probe(shifted);
    dfe_.snapshot_taps(start_taps);
    ComplexF probe_phase{0, 0};
    double mse = 0;
    double probe_energy = 0;
    int stat_rows = 0;
    for (int i = fb; i < k; ++i) {
        fill_window(static_cast<double>(probe_chip + i), window_.data());
        for (int j = 0; j < fb; ++j) past_[static_cast<std::size_t>(j)] = eq(pr[i - 1 - j]);
        const ComplexF y = cf(dfe_.equalise(window, probe_past));
        probe_phase = probe_phase + (y * dsp::conj(pr[i]));
        mse += static_cast<double>(dsp::mag2(y - pr[i]));
        probe_energy += static_cast<double>(dsp::mag2(cf(window_[static_cast<std::size_t>(ff_lead_)])));
        ++stat_rows;
        dfe_.add_training_row(window, probe_past, eq(pr[i]), 6.0f);
    }

    // Collapse detection (the C# phase-b-plan B2.1c): two consecutive probes uncorrelated with
    // the taps while their energy says the signal is there; one fresh solve per unhealthy
    // episode, re-armed by a healthy probe.
    const double pre_mse = mse / stat_rows;
    const double probe_energy_mean = probe_energy / stat_rows;
    const double probe_gain = static_cast<double>(dsp::abs(probe_phase) / static_cast<float>(k));
    const bool collapsed = collapse_armed_ && probe_gain < std::max(0.10, 0.45 * probe_gain_ref_) &&
                           probe_energy_mean >= 0.25 * probe_energy_ref_;
    collapsed_probes_ = collapsed ? collapsed_probes_ + 1 : 0;
    const bool fresh_solve = collapsed_probes_ >= 2;
    if (fresh_solve) {
        // Discard the accumulator (the collapsed frame's rows are poison), rebuild the probe rows
        // and zero the taps, so the anchored ridge becomes a plain ridge toward zero.
        dfe_.begin_training();
        for (int i = fb; i < k; ++i) {
            fill_window(static_cast<double>(probe_chip + i), window_.data());
            for (int j = 0; j < fb; ++j) past_[static_cast<std::size_t>(j)] = eq(pr[i - 1 - j]);
            dfe_.add_training_row(window, probe_past, eq(pr[i]), 6.0f);
        }
        std::fill(taps_cur_.begin(), taps_cur_.begin() + n_taps, EqSample{});
        dfe_.load_taps({taps_cur_.data(), static_cast<std::size_t>(n_taps)});
        ++stats_.collapse_resolves;
        collapsed_probes_ = 0;
        collapse_armed_ = false;
    }

    dfe_.seed_rls_from_training(track_ridge_, 1.0f, 0.0f);
    dfe_.solve_training(track_ridge_, true, 0.0f);

    // Per-probe phase re-anchor: re-equalise the probe with the new taps and take out the
    // residual common phase, unless the probe is below the 0.10 correlation floor.
    ComplexF post_phase{0, 0};
    for (int i = fb; i < k; ++i) {
        fill_window(static_cast<double>(probe_chip + i), window_.data());
        for (int j = 0; j < fb; ++j) past_[static_cast<std::size_t>(j)] = eq(pr[i - 1 - j]);
        post_phase = post_phase + (cf(dfe_.equalise(window, probe_past)) * dsp::conj(pr[i]));
    }
    if (static_cast<double>(dsp::abs(post_phase) / static_cast<float>(stat_rows)) >= 0.10) {
        dfe_.rotate_taps(eq(dsp::from_phase(-dsp::arg(post_phase))));
    }

    dfe_.snapshot_taps(end_taps);
    if (fresh_solve) {
        // The old anchor is the collapsed solution: hold the fresh solve over this frame.
        std::copy(end_taps.begin(), end_taps.end(), start_taps.begin());
    }

    // Residual carrier trim from the mean tap rotation between consecutive probe solves.
    ComplexF tap_rotation{0, 0};
    for (int i = 0; i < n_taps; ++i) {
        tap_rotation = tap_rotation + (cf(end_taps[static_cast<std::size_t>(i)]) * dsp::conj(cf(start_taps[static_cast<std::size_t>(i)])));
    }
    const bool rotated = static_cast<double>(dsp::mag2(tap_rotation)) > 1e-12;
    if (rotated) {
        const int frame_t2 = 2 * (mode.u + k);
        double delta_omega = -0.1 * static_cast<double>(dsp::arg(tap_rotation)) / frame_t2;
        const double window_3hz = 2.0 * pi * 3.0 / (2.0 * tables::symbol_rate);
        delta_omega = clamp(delta_omega, (omega_acquired_ - window_3hz) - omega_, (omega_acquired_ + window_3hz) - omega_);
        retune_carrier((2.0 * static_cast<double>(probe_chip + (k / 2))) + tau_, 0, delta_omega);
    }

    // Rows for the NEXT solve start here: decision-directed rows join the next probe's.
    dfe_.begin_training();

    // Fading statistic: the fractional tap change per frame beyond the common rotation, against a
    // min-tracking floor, with hysteresis. A fresh re-solve frame is left out.
    double tap_change = 0;
    if (!fresh_solve) {
        double start_norm = 0;
        double change_norm = 0;
        if (rotated) {
            const ComplexF rot = tap_rotation * static_cast<float>(1.0 / static_cast<double>(dsp::abs(tap_rotation)));
            for (int i = 0; i < n_taps; ++i) {
                const ComplexF s = cf(start_taps[static_cast<std::size_t>(i)]);
                start_norm += static_cast<double>(dsp::mag2(s));
                change_norm += static_cast<double>(dsp::mag2(cf(end_taps[static_cast<std::size_t>(i)]) - (rot * s)));
            }
        }
        tap_change = start_norm > 1e-12 ? change_norm / start_norm : 0;
        if (!fade_floor_seeded_) {
            fade_floor_ = tap_change;
            fade_floor_seeded_ = true;
        } else {
            fade_floor_ = std::min(tap_change, (fade_floor_ * 1.05) + 1e-4);
        }
        const bool excursion = tap_change > static_cast<double>(fade_excursion_ratio) * fade_floor_;
        if (excursion) {
            if (frames_since_excursion_ <= fade_enter_window_frames) fading_ = true;
            frames_since_excursion_ = 0;
        } else if (++frames_since_excursion_ > fade_exit_frames) {
            fading_ = false;
        }
    }

    // The path latches per burst: a channel that faded (or collapsed) stays on the fading path.
    fading_latched_ = fading_latched_ || fading_ || fresh_solve;
    const bool fading = fading_latched_;
    const float rls_weight = fading ? 1.0f : 0.1f;

    scrambler_.reset();
    const float dd_gate = dd_gate_radius(mode.modulation);

    // Probe-anchored tap trajectory for fading frames: the anchors' common rotation phi is
    // applied as a phase ramp over linearly interpolated de-rotated taps.
    double phi = 0;
    if (fading) {
        phi = rotated ? static_cast<double>(dsp::arg(tap_rotation)) : 0.0;
        const ComplexF derot = dsp::from_phase(-static_cast<float>(phi));
        for (int i = 0; i < n_taps; ++i) traj_end_[static_cast<std::size_t>(i)] = eq(cf(end_taps[static_cast<std::size_t>(i)]) * derot);
    }
    const std::span<const EqSample> taps_prev{taps_prev_.data(), static_cast<std::size_t>(n_taps)};
    const std::span<const EqSample> taps_cur{taps_cur_.data(), static_cast<std::size_t>(n_taps)};

    if (mode.modulation == Modulation::Qam16) {
        if (fading) {
            tap_trajectory(start_taps_.data(), traj_end_.data(), phi, frame_fraction(mode, 0), taps_cur_.data(), n_taps);
            dfe_.load_taps(taps_cur);
        }
        for (int u = 0; u < mode.u; ++u) {
            if (fading && u > 0) {
                std::copy(taps_cur_.begin(), taps_cur_.begin() + n_taps, taps_prev_.begin());
                tap_trajectory(start_taps_.data(), traj_end_.data(), phi, frame_fraction(mode, u), taps_cur_.data(), n_taps);
                dfe_.translate_taps(taps_prev, taps_cur);
            }
            fill_window(static_cast<double>(frame_chip_ + u), window_.data());
            const int scramble_nibble = scrambler_.next_qam(0, 4);
            const ComplexF y = cf(dfe_.equalise(window, decisions));
            const ComplexF clean = slice(y, mode.modulation);
            push_max_log_llrs(y, constellations().qam16.data(), 16, nullptr, 4, 10.0f, scramble_nibble);
            if (dsp::mag2(y - clean) < dd_gate) {
                dfe_.rls_update(window, decisions, eq(clean), rls_weight);
                dfe_.add_training_row(window, decisions, eq(clean), 0.25f);
            }
            push_decision(clean);
        }
    } else if (!fading) {
        // Three passes from three tap seeds (end, start and midpoint of the probe-to-probe
        // trajectory), outputs averaged; every pass starts from the frame's own decision history.
        std::copy(decisions_.begin(), decisions_.begin() + fb, frame_start_decisions_.begin());
        for (int u = 0; u < mode.u; ++u) {
            fill_window(static_cast<double>(frame_chip_ + u), window_.data());
            const ComplexF rotor = psk8(scrambler_.next_psk(0));
            const ComplexF y = cf(dfe_.equalise(window, decisions));
            const ComplexF descrambled = y * dsp::conj(rotor);
            const ComplexF clean = slice(descrambled, mode.modulation);
            pass1_[static_cast<std::size_t>(u)] = descrambled;
            dfe_.rls_update(window, decisions, eq(clean * rotor), rls_weight);
            if (dsp::mag2(descrambled - clean) < dd_gate) {
                dfe_.add_training_row(window, decisions, eq(clean * rotor), 0.25f);
            }
            push_decision(clean * rotor);
        }

        dfe_.load_taps(start_taps);
        std::copy(frame_start_decisions_.begin(), frame_start_decisions_.begin() + fb, decisions_.begin());
        scrambler_.reset();
        for (int u = 0; u < mode.u; ++u) {
            fill_window(static_cast<double>(frame_chip_ + u), window_.data());
            const ComplexF rotor = psk8(scrambler_.next_psk(0));
            const ComplexF y = cf(dfe_.equalise(window, decisions));
            const ComplexF descrambled = y * dsp::conj(rotor);
            const ComplexF clean = slice(descrambled, mode.modulation);
            pass2_[static_cast<std::size_t>(u)] = descrambled;
            dfe_.rls_update(window, decisions, eq(clean * rotor), rls_weight);
            push_decision(clean * rotor);
        }

        for (int i = 0; i < n_taps; ++i) {
            taps_cur_[static_cast<std::size_t>(i)] =
                eq((cf(start_taps[static_cast<std::size_t>(i)]) + cf(end_taps[static_cast<std::size_t>(i)])) * 0.5f);
        }
        dfe_.load_taps(taps_cur);
        std::copy(frame_start_decisions_.begin(), frame_start_decisions_.begin() + fb, decisions_.begin());
        scrambler_.reset();
        for (int u = 0; u < mode.u; ++u) {
            fill_window(static_cast<double>(frame_chip_ + u), window_.data());
            const ComplexF rotor = psk8(scrambler_.next_psk(0));
            const ComplexF y = cf(dfe_.equalise(window, decisions));
            const ComplexF descrambled = y * dsp::conj(rotor);
            const ComplexF clean = slice(descrambled, mode.modulation);
            const ComplexF averaged =
                (pass1_[static_cast<std::size_t>(u)] + pass2_[static_cast<std::size_t>(u)] + descrambled) * (1.0f / 3.0f);
            push_llrs(averaged);
            dfe_.rls_update(window, decisions, eq(clean * rotor), rls_weight);
            push_decision(clean * rotor);
        }
    } else {
        // Fading: one pass with the tap base interpolated between the bracketing probe solves and
        // the RLS deviation riding on top, decision-confidence gated.
        tap_trajectory(start_taps_.data(), traj_end_.data(), phi, frame_fraction(mode, 0), taps_cur_.data(), n_taps);
        dfe_.load_taps(taps_cur);
        for (int u = 0; u < mode.u; ++u) {
            if (u > 0) {
                std::copy(taps_cur_.begin(), taps_cur_.begin() + n_taps, taps_prev_.begin());
                tap_trajectory(start_taps_.data(), traj_end_.data(), phi, frame_fraction(mode, u), taps_cur_.data(), n_taps);
                dfe_.translate_taps(taps_prev, taps_cur);
            }
            fill_window(static_cast<double>(frame_chip_ + u), window_.data());
            const ComplexF rotor = psk8(scrambler_.next_psk(0));
            const ComplexF y = cf(dfe_.equalise(window, decisions));
            const ComplexF descrambled = y * dsp::conj(rotor);
            const ComplexF clean = slice(descrambled, mode.modulation);
            push_llrs(descrambled);
            if (dsp::mag2(descrambled - clean) < dd_gate) {
                dfe_.rls_update(window, decisions, eq(clean * rotor), rls_weight);
                dfe_.add_training_row(window, decisions, eq(clean * rotor), 0.25f);
            }
            push_decision(clean * rotor);
        }
    }

    dfe_.symmetrize_p(10.0f);
    dfe_.load_taps(end_taps);
    for (int i = 0; i < k; ++i) push_decision(pr[i]);

    probe_mse_ = (0.7 * probe_mse_) + (0.3 * pre_mse);

    track_probe_timing(probe_chip, pr);

    // The probe one frame back: every lag of it is resident however the input was cut into
    // blocks, so the statistic does not depend on the caller's chunk size.
    double plain_presence = 0;
    const double presence = probe_presence(presence_probe_chip_, probe(presence_probe_shifted_), plain_presence);
    presence_probe_chip_ = probe_chip;
    presence_probe_shifted_ = shifted;
    ++stats_.frames;

    if (trace_fn_ != nullptr) {
        DfeFrameTrace& t = trace_;
        t.frame_chip = frame_chip_;
        t.at_t2 = written_;
        t.probe_gain = probe_gain;
        t.pre_mse = pre_mse;
        t.probe_energy = probe_energy_mean;
        t.probe_gain_ref = probe_gain_ref_;
        t.probe_energy_ref = probe_energy_ref_;
        t.probe_mse = probe_mse_;
        t.tap_change = tap_change;
        t.fade_floor = fade_floor_;
        t.omega = omega_;
        t.omega_acquired = omega_acquired_;
        t.theta_base = theta_base_;
        t.tau = tau_;
        t.presence = presence;
        t.presence_plain = plain_presence;
        t.pattern_evidence = pattern_.evidence();
        t.fading = fading_;
        t.fading_latched = fading;
        t.fresh_solve = fresh_solve;
        t.shifted_probe = shifted;
        t.bad_probes = bad_probes_;
        t.frames_since_excursion = frames_since_excursion_;
        t.ff_energy = dfe_.ff_energy();
        t.tap_count = n_taps;
        dfe_.snapshot_taps({t.taps.data(), static_cast<std::size_t>(n_taps)});
        trace_fn_(trace_context_, t);
    }

    if (probe_gain < std::max(0.10, 0.45 * probe_gain_ref_)) {
        // Signal-lost patience is about four seconds of wall clock, whatever the frame length.
        const int bad_probe_limit = static_cast<int>(std::ceil(4.0 * tables::symbol_rate / (mode.u + k)));
        if (++bad_probes_ >= bad_probe_limit) {
            end_burst(DfeEndReason::SignalLost, burst_bits_);
            return;
        }
    } else {
        bad_probes_ = 0;
        probe_gain_ref_ = (0.95 * probe_gain_ref_) + (0.05 * probe_gain);
        probe_energy_ref_ = (0.95 * probe_energy_ref_) + (0.05 * probe_energy_mean);
        collapse_armed_ = true;
    }

    if (signal_absent(presence)) {
        ++stats_.signal_absent_releases;
        end_burst(DfeEndReason::SignalAbsent, burst_bits_);
        return;
    }

    if (block_frames_ < max_block_frames) block_frame_chips_[static_cast<std::size_t>(block_frames_++)] = frame_chip_;
    frame_chip_ += mode.u + k;
    ++frame_in_block_;
    if (frame_in_block_ == il_.frames) {
        frame_in_block_ = 0;
        finish_block();
        block_frames_ = 0;
    }
}

// ------------------------------------------------------------------ signal-absent release

void DfeReceiver::start_presence_window() noexcept {
    const int frames =
        static_cast<int>(std::ceil(presence_window_seconds * tables::symbol_rate / (mode_.u + mode_.k)));
    presence_ring_len_ = std::min(frames, max_presence_frames);
    std::fill(presence_ring_.begin(), presence_ring_.end(), 0.0);
    presence_frames_ = 0;
    presence_sum_ = 0;
    const int lags = presence_lags(mode_.k);
    presence_threshold_ = presence_sigmas * std::sqrt(lags / static_cast<double>(frames));

    // The first frame scores the probe that ends the preamble (unshifted).
    presence_probe_chip_ = data_start_chip_;
    presence_probe_shifted_ = false;
    presence_background_ = 0;
    presence_frame_seconds_ = (mode_.u + mode_.k) / static_cast<double>(tables::symbol_rate);

    // The lag spectrum: 4 bins per lag across the full circle, and its DFT kernel e^{-j w d},
    // d counted from the middle lag.
    const int bins = 4 * lags;
    presence_lags_ = lags;
    presence_bins_ = bins;
    std::fill(presence_spectrum_.begin(), presence_spectrum_.end(), 0.0);
    presence_spectrum_seeded_ = false;
    const double centre = (lags - 1) / 2.0;
    for (int j = 0; j < bins; ++j) {
        const double w = presence_bin_frequency(j, bins);
        for (int d = 0; d < lags; ++d) {
            presence_dft_re_[(j * lags) + d] = std::cos(w * (d - centre));
            presence_dft_im_[(j * lags) + d] = -std::sin(w * (d - centre));
        }
    }
}

// The C# ProbePresence: how much more coherent mini-probe energy the received symbols hold than
// the background would put there, with up to four steady carriers found in the averaged lag
// spectrum projected off. Reads the ring through the receiver's carrier and timing, writes no
// receiver state other than its own averages.
double DfeReceiver::probe_presence(std::int64_t probe_chip, const ComplexF* pr, double& plain) noexcept {
    const int k = mode_.k;
    const int lags = presence_lags_;
    const int first = -(lags - 1) / 2;
    const int span = k + lags - 1;
    std::array<ComplexF, max_k + max_lags> received{};
    double power = 0;
    for (int i = 0; i < span; ++i) {
        received[static_cast<std::size_t>(i)] = read_chip(static_cast<double>(probe_chip + first + i));
        power += static_cast<double>(dsp::mag2(received[static_cast<std::size_t>(i)]));
    }
    power /= span;
    if (power <= 0) {
        plain = 0;
        return 0;
    }

    const double scale = 1.0 / std::sqrt(k * power);
    std::array<double, max_lags> re{};
    std::array<double, max_lags> im{};
    double energy = 0;
    for (int d = 0; d < lags; ++d) {
        ComplexF c{0, 0};
        for (int i = 0; i < k; ++i) c = c + (received[static_cast<std::size_t>(i + d)] * dsp::conj(pr[i]));
        re[static_cast<std::size_t>(d)] = static_cast<double>(c.re) * scale;
        im[static_cast<std::size_t>(d)] = static_cast<double>(c.im) * scale;
        energy += (re[static_cast<std::size_t>(d)] * re[static_cast<std::size_t>(d)]) +
                  (im[static_cast<std::size_t>(d)] * im[static_cast<std::size_t>(d)]);
    }
    plain = energy - lags;

    // This frame's lag spectrum, before anything is removed, for the frames after it.
    const int bins = presence_bins_;
    std::array<double, max_bins> spectrum{};
    for (int j = 0; j < bins; ++j) {
        double x_re = 0;
        double x_im = 0;
        const int row = j * lags;
        for (int d = 0; d < lags; ++d) {
            const double k_re = presence_dft_re_[row + d];
            const double k_im = presence_dft_im_[row + d];
            x_re += (re[static_cast<std::size_t>(d)] * k_re) - (im[static_cast<std::size_t>(d)] * k_im);
            x_im += (re[static_cast<std::size_t>(d)] * k_im) + (im[static_cast<std::size_t>(d)] * k_re);
        }
        spectrum[static_cast<std::size_t>(j)] = (x_re * x_re) + (x_im * x_im);
    }

    // The carriers the earlier frames point to, and the orthonormal basis they span.
    std::array<double, presence_max_carriers> carriers{};
    const int found = presence_spectrum_seeded_ ? find_presence_carriers(lags, carriers.data()) : 0;
    std::array<double, 2 * presence_max_carriers * max_lags> basis_re{};
    std::array<double, 2 * presence_max_carriers * max_lags> basis_im{};
    int rank = 0;
    const double centre = (lags - 1) / 2.0;
    for (int n = 0; n < 2 * found; ++n) {
        const double w = carriers[static_cast<std::size_t>(n / 2)];
        double* v_re = &basis_re[static_cast<std::size_t>(rank * lags)];
        double* v_im = &basis_im[static_cast<std::size_t>(rank * lags)];
        for (int d = 0; d < lags; ++d) {
            const double weight = n % 2 == 0 ? 1.0 : d - centre;
            v_re[d] = weight * std::cos(w * (d - centre));
            v_im[d] = weight * std::sin(w * (d - centre));
        }
        // Modified Gram-Schmidt against the vectors already in the basis.
        for (int q = 0; q < rank; ++q) {
            const double* q_re = &basis_re[static_cast<std::size_t>(q * lags)];
            const double* q_im = &basis_im[static_cast<std::size_t>(q * lags)];
            double dot_re = 0;
            double dot_im = 0;
            for (int d = 0; d < lags; ++d) {
                dot_re += (q_re[d] * v_re[d]) + (q_im[d] * v_im[d]);
                dot_im += (q_re[d] * v_im[d]) - (q_im[d] * v_re[d]);
            }
            for (int d = 0; d < lags; ++d) {
                v_re[d] -= (dot_re * q_re[d]) - (dot_im * q_im[d]);
                v_im[d] -= (dot_re * q_im[d]) + (dot_im * q_re[d]);
            }
        }
        double norm = 0;
        for (int d = 0; d < lags; ++d) norm += (v_re[d] * v_re[d]) + (v_im[d] * v_im[d]);
        if (norm < 1e-6) continue;  // already spanned
        const double inv = 1.0 / std::sqrt(norm);
        for (int d = 0; d < lags; ++d) {
            v_re[d] *= inv;
            v_im[d] *= inv;
        }
        ++rank;
    }

    // Project the carriers off this frame, and keep each lag's remaining noise variance.
    std::array<double, max_lags> variance{};
    std::fill(variance.begin(), variance.begin() + lags, 1.0);
    for (int q = 0; q < rank; ++q) {
        const double* q_re = &basis_re[static_cast<std::size_t>(q * lags)];
        const double* q_im = &basis_im[static_cast<std::size_t>(q * lags)];
        double dot_re = 0;
        double dot_im = 0;
        for (int d = 0; d < lags; ++d) {
            dot_re += (q_re[d] * re[static_cast<std::size_t>(d)]) + (q_im[d] * im[static_cast<std::size_t>(d)]);
            dot_im += (q_re[d] * im[static_cast<std::size_t>(d)]) - (q_im[d] * re[static_cast<std::size_t>(d)]);
        }
        for (int d = 0; d < lags; ++d) {
            re[static_cast<std::size_t>(d)] -= (dot_re * q_re[d]) - (dot_im * q_im[d]);
            im[static_cast<std::size_t>(d)] -= (dot_re * q_im[d]) + (dot_im * q_re[d]);
            variance[static_cast<std::size_t>(d)] -= (q_re[d] * q_re[d]) + (q_im[d] * q_im[d]);
        }
    }

    std::array<double, max_lags> lag_energy{};
    double residual = 0;
    for (int d = 0; d < lags; ++d) {
        const double e = (re[static_cast<std::size_t>(d)] * re[static_cast<std::size_t>(d)]) +
                         (im[static_cast<std::size_t>(d)] * im[static_cast<std::size_t>(d)]);
        residual += e;
        lag_energy[static_cast<std::size_t>(d)] = e / std::max(variance[static_cast<std::size_t>(d)], 0.05);
    }

    // The median residual lag over the median of that many unit exponentials.
    std::sort(lag_energy.begin(), lag_energy.begin() + lags);
    const double background = lag_energy[static_cast<std::size_t>(lags / 2)] / expected_median_of_unit_exponentials(lags);
    const double level = presence_background_ > 0 ? presence_background_ : background;
    const double presence = (residual / level) - (lags - rank);

    // Fold this frame into the averages for the frames after it.
    const double frame_seconds = presence_frame_seconds_;
    const double beta = std::min(1.0, frame_seconds / presence_background_seconds);
    presence_background_ =
        presence_background_ > 0 ? presence_background_ + (beta * (background - presence_background_)) : background;
    const double alpha = presence_spectrum_seeded_ ? std::min(1.0, frame_seconds / presence_spectrum_seconds) : 1.0;
    for (int j = 0; j < bins; ++j) {
        presence_spectrum_[static_cast<std::size_t>(j)] +=
            alpha * ((spectrum[static_cast<std::size_t>(j)] / level) - presence_spectrum_[static_cast<std::size_t>(j)]);
    }
    presence_spectrum_seeded_ = true;
    return presence;
}

// The C# FindPresenceCarriers: steady carriers in the averaged lag spectrum, strongest first.
int DfeReceiver::find_presence_carriers(int lags, double* carriers) const noexcept {
    const int bins = presence_bins_;
    std::array<double, max_bins> left{};
    std::array<double, max_bins> sorted{};
    std::copy(presence_spectrum_.begin(), presence_spectrum_.begin() + bins, left.begin());
    std::copy(presence_spectrum_.begin(), presence_spectrum_.begin() + bins, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + bins);
    const double median = sorted[static_cast<std::size_t>(bins / 2)];
    const double floor = std::max(static_cast<double>(lags), median);
    const double threshold = std::max(presence_carrier_over_noise * lags,
                                      lags + (presence_carrier_over_signal * std::max(0.0, median - lags)));
    int found = 0;
    while (found < presence_max_carriers) {
        int peak = 0;
        for (int j = 1; j < bins; ++j) {
            if (left[static_cast<std::size_t>(j)] > left[static_cast<std::size_t>(peak)]) peak = j;
        }
        if (left[static_cast<std::size_t>(peak)] <= threshold) break;

        const double below = left[static_cast<std::size_t>((peak + bins - 1) % bins)];
        const double at = left[static_cast<std::size_t>(peak)];
        const double above = left[static_cast<std::size_t>((peak + 1) % bins)];
        const double curvature = below - (2 * at) + above;
        const double offset = curvature < 0 ? clamp(0.5 * (below - above) / curvature, -0.5, 0.5) : 0;
        const double w = presence_bin_frequency(peak, bins) + (offset * 2.0 * pi / bins);
        carriers[found++] = w;

        const double excess = at - floor;
        for (int j = 0; j < bins; ++j) {
            const double delta = presence_bin_frequency(j, bins) - w;
            const double half = std::sin(delta / 2);
            const double kernel = std::abs(half) < 1e-9 ? lags : std::sin(lags * delta / 2) / half;
            left[static_cast<std::size_t>(j)] -= excess * kernel * kernel / (lags * static_cast<double>(lags));
        }
    }
    return found;
}

bool DfeReceiver::signal_absent(double presence) noexcept {
    const int len = presence_ring_len_;
    const int slot = presence_frames_ % len;
    if (presence_frames_ >= len) presence_sum_ -= presence_ring_[static_cast<std::size_t>(slot)];
    presence_ring_[static_cast<std::size_t>(slot)] = presence;
    presence_sum_ += presence;
    ++presence_frames_;
    return presence_frames_ >= len && presence_sum_ / len < presence_threshold_;
}

// ------------------------------------------------------------------ timing and carrier

void DfeReceiver::track_probe_timing(std::int64_t probe_chip, const ComplexF* pr) noexcept {
    double magnitudes[3];
    for (int d = 0; d < 3; ++d) {
        const double offset = (d - 1) * 0.5;
        ComplexF c{0, 0};
        for (int i = 0; i < mode_.k; ++i) {
            c = c + (read_t2((2.0 * static_cast<double>(probe_chip + i)) + offset) * dsp::conj(pr[i]));
        }
        magnitudes[d] = static_cast<double>(dsp::abs(c));
    }
    const double denom = magnitudes[0] - (2 * magnitudes[1]) + magnitudes[2];
    if (std::abs(denom) < 1e-9) return;
    // Slow slew only: real clock skew is ppm scale; anything faster is estimator noise.
    const double delta = 0.5 * (magnitudes[0] - magnitudes[2]) / denom * 0.5;
    tau_ += clamp(0.1 * delta, -0.03, 0.03);
}

// A phase and frequency correction with the phase model re-anchored at half_chips_ref, so an
// omega change re-rotates only the future.
void DfeReceiver::retune_carrier(double half_chips_ref, double delta_theta, double delta_omega) noexcept {
    omega_ += delta_omega;
    theta_base_ += delta_theta - (delta_omega * half_chips_ref);
}

// ------------------------------------------------------------------ soft values

void DfeReceiver::push_llrs(ComplexF d) noexcept {
    switch (mode_.modulation) {
        case Modulation::Bpsk:
            add_llr(4.0f * d.re);
            return;
        case Modulation::Psk8:
            // Max-log over 8 points; the bit label of ring symbol s is the tribit t with
            // transcode_8psk[t] == s.
            push_max_log_llrs(d, constellations().psk8.data(), 8, symbol_to_tribit8.data(), 3, 2.0f, 0);
            return;
        case Modulation::Qpsk:
            // Table D-IV Gray map: MSB 0 for {+1, +j}, LSB 0 for {+1, -j}.
            add_llr(2.0f * (d.re + d.im));
            add_llr(2.0f * (d.re - d.im));
            return;
        default:
            // QAM16 soft values come from the first-pass max-log call with its own scale; the
            // C# refuses this path (an exception) rather than mis-scale, and so does this.
            return;
    }
}

void DfeReceiver::push_max_log_llrs(ComplexF y, const ComplexF* constellation, int points,
                                    const std::uint8_t* labels, int bits, float scale, int scramble) noexcept {
    for (int b = 0; b < bits; ++b) {
        float min0 = 3.40282347e+38f;
        float min1 = 3.40282347e+38f;
        for (int s = 0; s < points; ++s) {
            const int label = labels != nullptr ? labels[s] : (s ^ scramble);
            const float d = dsp::mag2(y - constellation[s]);
            if (((label >> (bits - 1 - b)) & 1) == 0) {
                if (d < min0) min0 = d;
            } else {
                if (d < min1) min1 = d;
            }
        }
        add_llr(scale * (min1 - min0));
    }
}

void DfeReceiver::add_llr(float llr) noexcept {
    if (block_llr_count_ < static_cast<std::size_t>(il_.size_bits)) block_llrs_[block_llr_count_] = llr;
    ++block_llr_count_;
}

void DfeReceiver::push_decision(ComplexF wire_symbol) noexcept {
    for (int j = nfb_ - 1; j > 0; --j) decisions_[static_cast<std::size_t>(j)] = decisions_[static_cast<std::size_t>(j - 1)];
    if (nfb_ > 0) decisions_[0] = eq(wire_symbol);
}

// ------------------------------------------------------------------ blocks and the burst

void DfeReceiver::finish_block() noexcept {
    const std::size_t size = static_cast<std::size_t>(il_.size_bits);
    const std::size_t input = static_cast<std::size_t>(il_.input_bits);
    if (block_llr_count_ != size) {
        // The C# throws ("interleaver block LLR accounting error"); it cannot happen for the
        // waveforms implemented. End the burst rather than decode a partial block.
        end_burst(DfeEndReason::SignalLost, burst_bits_);
        return;
    }

    std::copy(block_llrs_, block_llrs_ + size, first_pass_llrs_);
    decode_block(block_llrs_, info_);

    // The turbo stage (the C# FinishBlock): for every equaliser mode but QAM16 the turbo
    // re-equalisation loop, with the frozen-probe salvage and the MFB ensemble for 8PSK; for QAM16
    // the MFB block decoder instead. Each keeps the first-pass decode when it finds no fixed point.
    block_.turbo = DfeTurbo::Skipped;
    block_.turbo_llrs = {};
    block_.mfb_offered = false;
    block_.mfb_selected = false;
    block_.relock_offers = 0;
    block_.relock_adopted = 0;
    const bool full = block_frames_ == il_.frames;
    if (config_.turbo && mode_.modulation != Modulation::Qam16 && full && block_samples_resident()) {
        // The rows gathered since the last probe solve belong to the next one; keep them.
        dfe_.snapshot_training();
        std::memcpy(first_info_, info_, input);
        bool aborted = false;
        const bool converged = run_turbo_loop(info_, prev_info_, aborted);
        if (!aborted) {
            std::copy(block_llrs_, block_llrs_ + size, turbo_llrs_);
            block_.turbo_llrs = {turbo_llrs_, size};
        }
        if (converged) {
            ++stats_.turbo_converged;
            block_.turbo = DfeTurbo::Converged;
        } else if (aborted) {
            ++stats_.turbo_aborted;
            block_.turbo = DfeTurbo::Aborted;
        } else if (mode_.modulation == Modulation::Psk8 &&
                   (try_salvage_revert(info_, prev_info_) || try_salvage_relock(info_, prev_info_))) {
            ++stats_.turbo_salvaged;
            ++stats_.turbo_converged;
            block_.turbo = DfeTurbo::Salvaged;
        } else {
            // No fixed point: a self-trained iterate is not evidence, keep the first pass.
            ++stats_.turbo_reverted;
            std::memcpy(info_, first_info_, input);
            block_.turbo = DfeTurbo::Reverted;
        }
        dfe_.restore_training();

        if (mode_.modulation == Modulation::Psk8) {
            // The 8PSK per-block ensemble: the MFB decode, when it converges, is weighed against
            // the chain path's in log-likelihood units and the larger evidence wins.
            if (!mfb_ready_) mfb_ready_ = mfb_.begin(mode_, il_, *code_, *puncture_, interleaver_, *viterbi_, *siso_);
            std::memcpy(mfb_info_, info_, input);
            if (mfb_ready_ && mfb_.decode_block(*this, block_frame_chips_.data(), mfb_info_)) {
                const double chain_price = mfb_.price(info_, block_frame_chips_.data());
                const double mfb_price = mfb_.price(mfb_info_, block_frame_chips_.data());
                const double mfb_evidence = static_cast<double>(mfb_.price_rows()) * std::log(chain_price / mfb_price);
                double chain_evidence = 0;
                int diff_bits = 0;
                ++stats_.mfb_offered;
                block_.mfb_offered = true;
                if (mfb_evidence > 0 && block_llr_count_ == size) {
                    encode_block(info_, fetched_);
                    encode_block(mfb_info_, fetched2_);
                    for (std::size_t i = 0; i < size; ++i) {
                        if (fetched_[i] != fetched2_[i]) {
                            ++diff_bits;
                            chain_evidence += static_cast<double>(std::abs(block_llrs_[i]));
                        }
                    }
                    if (diff_bits > 0 && mfb_evidence > chain_evidence) {
                        std::memcpy(info_, mfb_info_, input);
                        ++stats_.mfb_selected;
                        block_.mfb_selected = true;
                    }
                }
            }
        }
    } else if (config_.turbo && mode_.modulation == Modulation::Qam16 && full && block_samples_resident()) {
        if (!mfb_ready_) mfb_ready_ = mfb_.begin(mode_, il_, *code_, *puncture_, interleaver_, *viterbi_, *siso_);
        std::memcpy(first_info_, info_, input);
        if (mfb_ready_ && mfb_.decode_block(*this, block_frame_chips_.data(), info_)) {
            ++stats_.turbo_converged;
            block_.turbo = DfeTurbo::Converged;
        } else {
            ++stats_.turbo_reverted;
            std::memcpy(info_, first_info_, input);
            block_.turbo = DfeTurbo::Reverted;
        }
    } else {
        ++stats_.turbo_skipped;
    }

    block_.index = block_index_;
    block_.at_t2 = written_;
    block_.info = {info_, input};
    block_.llrs = {first_pass_llrs_, size};
    pending_block_ = true;
    block_llr_count_ = 0;

    // The EOM scan of the C# FindEom over the burst's bits, starting 31 bits before this block.
    const std::int64_t before = burst_bits_;
    const std::int64_t search_from = std::max<std::int64_t>(0, before - 31);
    std::uint32_t window = 0;
    std::int64_t eom = -1;
    const std::int64_t total = before + static_cast<std::int64_t>(input);
    for (std::int64_t i = search_from; i < total; ++i) {
        const std::int64_t back = before - i;  // > 0: from the tail of earlier blocks
        const std::uint8_t bit = back > 0 ? eom_tail_[static_cast<std::size_t>(eom_tail_len_ - back)]
                                          : info_[static_cast<std::size_t>(i - before)];
        window = (window << 1) | bit;
        if (i - search_from >= 31 && window == tables::eom) {
            eom = i - 31;
            break;
        }
    }
    burst_bits_ = total;
    // Keep the last 31 bits for the next block's scan.
    std::array<std::uint8_t, 31> tail{};
    int len = 0;
    for (std::int64_t i = std::max<std::int64_t>(0, total - 31); i < total; ++i) {
        const std::int64_t back = before - i;
        tail[static_cast<std::size_t>(len++)] = back > 0 ? eom_tail_[static_cast<std::size_t>(eom_tail_len_ - back)]
                                                         : info_[static_cast<std::size_t>(i - before)];
    }
    eom_tail_ = tail;
    eom_tail_len_ = len;
    ++block_index_;

    if (eom >= 0) {
        end_burst(DfeEndReason::Eom, eom);
        return;
    }
    if (config_.max_input_data_blocks > 0 && block_index_ >= config_.max_input_data_blocks) {
        end_burst(DfeEndReason::MaxInputDataBlocks, burst_bits_);
    }
}

void DfeReceiver::end_burst(DfeEndReason reason, std::int64_t payload_bits) noexcept {
    end_.reason = reason;
    end_.blocks = block_index_;
    end_.payload_bits = payload_bits;
    end_.at_t2 = written_;
    pending_end_ = true;
    active_ = false;
    tracking_initialized_ = false;
    agc_gain_ = 1.0f;
    pattern_.reset();  // the C# EndBurst forgets the evidence
}

}  // inline namespace

}  // namespace m0lte::ms110d
