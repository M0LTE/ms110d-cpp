// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The equaliser receive path for waveforms 1 to 8 and 13: everything the C# Ms110dDemodulator
// does after the preamble detector has handed over a burst, up to the decoded blocks and the end
// of the burst. Ported from M0LTE.Ms110d 0.2.1, step for step:
//
//   InitializeDfe   tap counts and ridges per probe length, the least-squares solve over the
//                   last preamble super-frame and the first mini-probe, RLS seeding, the probe
//                   statistics the collapse and signal-lost tests are measured against;
//   ProcessFrame    per frame: the probe pattern score where the two shifted-probe conventions
//                   disagree (ProbePattern), the probe-anchored solve, collapse detection and the
//                   fresh re-solve, the phase re-anchor, the carrier trim from the tap rotation,
//                   the fading statistic, the data span by the three-pass static path, the
//                   probe-anchored tap trajectory (fading) or the QAM path, soft values,
//                   probe timing, the probe presence statistic and the signal-lost and
//                   signal-absent exits;
//   FinishBlock     deinterleave, depuncture, tail-biting Viterbi, then the turbo stage
//                   (dfe_turbo.cpp: the turbo re-equalisation loop, the 8PSK salvage rungs and
//                   per-block MFB ensemble; the MFB block decoder for QAM16, mfb_block_decoder.*),
//                   the EOM scan and the block limit. The turbo stage saves and restores every piece
//                   of tracking state it touches, so the first pass is the same with it on or off.
//
// Not here: the preamble search (preamble_detector.hpp, whose detection is the input), the WN 0
// path (wn0_receiver.hpp), the C#'s diagnostic instruments (genie, oracle, truth injection and the
// rest), which are off in normal use and leave the shipped path bit-identical, and the terminate
// command (a later story, with the public receive API).
//
// Precision: exactly the C#'s, in both builds. Samples, the equaliser, decisions and soft values
// are single precision (the C# Complex32 and float); the carrier and timing model, the probe
// statistics, the fading and presence statistics are double. The turbo stage's chain BCJR is
// float and the MFB decoder double, as the C#'s. The only `real` is inside the equaliser
// (docs/dev/equaliser.md). See docs/dev/dfe-receiver.md.
//
// Memory: one block of caller storage (storage_bytes()), sized at configure from the longest
// interleaver the receiver is to accept, plus the object (about 125 KB). No allocation after
// configure, no exceptions. The decoders (Viterbi, SISO, chain BCJR) are the caller's.

#include <m0lte/ms110d/mode.hpp>

#include "dsp/complex_f.hpp"
#include "equaliser.hpp"
#include "bcjr.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "mfb_block_decoder.hpp"
#include "interleaver.hpp"
#include "probe_pattern.hpp"
#include "puncture.hpp"
#include "rx_front_end.hpp"
#include "scrambler.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

struct PreambleDetection;

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

struct DfeReceiverConfig {
    // Roll-off of the receive matched filter (the C# Ms110dDemodOptions.RollOff).
    double roll_off = default_roll_off;
    // The longest interleaver accepted: storage is sized for it, and start() refuses a burst with
    // a longer one. Long accepts everything (and needs the C#'s 65536 sample ring).
    InterleaverKind max_interleaver = InterleaverKind::Long;
    // Stop after this many input-data blocks, 0 for no limit (D.5.4.5.3, the C#
    // MaxInputDataBlocks).
    int max_input_data_blocks = 0;
    // The turbo re-equalisation and the MFB block decoder after each block's first decode (the
    // C# DisableTurbo, inverted). Off decodes from the first-pass soft values alone.
    bool turbo = true;
};

// The decoders the receiver uses, the caller's (each allocates its tables when constructed):
// tail-biting Viterbi and SISO for K = 7 and K = 9, built for at least max_input_bits_for() info
// bits, and the chain BCJR, built for at least max_frame_symbols symbols and 16 points. The chain
// BCJR is single precision in both builds, the C#'s own (docs/dev/dfe-receiver.md).
struct DfeDecoders {
    fec::TailBitingViterbiDecoder* viterbi_k7 = nullptr;
    fec::TailBitingViterbiDecoder* viterbi_k9 = nullptr;
    fec::TailBitingSisoDecoder* siso_k7 = nullptr;
    fec::TailBitingSisoDecoder* siso_k9 = nullptr;
    BasicChainBcjr<float>* chain = nullptr;
};

// Where a burst is and what it is, as the preamble detector leaves it (from_detection()).
struct DfeStart {
    int waveform = 1;
    InterleaverKind interleaver = InterleaverKind::Short;
    int constraint_length = 7;
    double chip0 = 0;              // T/2 position of chip 0 of the matched super-frame
    double carrier_omega = 0;      // phase increment per T/2 sample, after the final re-fit
    double carrier_theta_base = 0; // carrier phase at chip0
    float agc_gain = 1.0f;         // the level gain every read goes through
    std::int64_t data_start_chip = 0;  // chips from chip0 to the first data symbol
};

DfeStart dfe_start_from_detection(const PreambleDetection& detection) noexcept;

// Why a burst ended (the C# Ms110dBurstEndReason, the values that can arise here).
enum class DfeEndReason { Eom, MaxInputDataBlocks, SignalLost, SignalAbsent };

// What one frame did: the C# FrameDiagnostics point, for tests and diagnostics.
struct DfeFrameTrace {
    std::int64_t frame_chip = 0;     // first data chip of the frame
    std::int64_t at_t2 = 0;          // T/2 samples received when the frame was processed
    double probe_gain = 0;           // |sum y conj(p)| / K before the solve
    double pre_mse = 0;
    double probe_energy = 0;
    double probe_gain_ref = 0;       // the references as they stand at the trace (before update)
    double probe_energy_ref = 0;
    double probe_mse = 0;
    double tap_change = 0;
    double fade_floor = 0;
    double omega = 0;
    double omega_acquired = 0;       // the carrier at the data start: the trim's clamp centre
    double theta_base = 0;
    double tau = 0;
    double presence = 0;
    double presence_plain = 0;
    double pattern_evidence = 0;
    bool fading = false;             // the excursion detector's state
    bool fading_latched = false;     // the path taken
    bool fresh_solve = false;
    bool shifted_probe = false;
    int bad_probes = 0;
    int frames_since_excursion = 0;  // the fading detector's counter
    float ff_energy = 0;
    std::array<EqSample, eq_max_taps> taps{};  // the taps after the frame (feed-forward then feedback)
    int tap_count = 0;
};

using DfeFrameTraceFn = void (*)(void* context, const DfeFrameTrace& trace) noexcept;

// What the turbo stage did with a block (the C# TurboConverged, Reverted and so on).
enum class DfeTurbo {
    Skipped,    // not run: turbo off, a partial block, or samples no longer resident
    Converged,  // reached a decode fixed point (the decode is the fixed point)
    Salvaged,   // 8PSK: no fixed point, but the frozen-probe salvage found one
    Reverted,   // no fixed point: the first-pass decode stands
    Aborted,    // the samples ran out part way through: the decode before it stands
};

// A decoded block, valid from the event until the receiver is pushed again.
struct DfeBlock {
    int index = 0;                         // block number within the burst, from 0
    std::int64_t at_t2 = 0;                // T/2 samples received when it was decoded
    std::span<const std::uint8_t> info;    // the decoded info bits, one per byte
    std::span<const float> llrs;           // the first-pass soft values, wire (fetched) order
    DfeTurbo turbo = DfeTurbo::Skipped;
    // The last turbo iterate's soft values (the C# TurboBlockLlrs), empty when the turbo loop
    // did not run or was aborted.
    std::span<const float> turbo_llrs;
    bool mfb_offered = false;              // 8PSK: the MFB decode converged and was weighed
    bool mfb_selected = false;             // ... and kept over the chain path's
    int relock_offers = 0;                 // 8PSK salvage: frames offered the late-lock geometry
    int relock_adopted = 0;                // ... and frames that took it
};

struct DfeBurstEnd {
    DfeEndReason reason = DfeEndReason::SignalLost;
    int blocks = 0;                        // input-data blocks decoded
    std::int64_t payload_bits = 0;         // bits before the EOM, or every decoded bit without one
    std::int64_t at_t2 = 0;                // T/2 samples received when the burst ended
};

enum class DfeEvent { None, BlockDecoded, BurstEnded };

// Counts since configure() or reset().
struct DfeReceiverStats {
    std::int64_t collapse_resolves = 0;
    std::int64_t signal_absent_releases = 0;
    std::int64_t frames = 0;
    std::int64_t turbo_converged = 0;   // includes the salvaged ones, as the C#
    std::int64_t turbo_reverted = 0;
    std::int64_t turbo_salvaged = 0;
    std::int64_t turbo_aborted = 0;
    std::int64_t turbo_skipped = 0;
    std::int64_t mfb_offered = 0;
    std::int64_t mfb_selected = 0;
};

class DfeReceiver {
public:
    DfeReceiver() noexcept = default;
    DfeReceiver(const DfeReceiver&) = delete;
    DfeReceiver& operator=(const DfeReceiver&) = delete;

    // Bytes of storage configure() needs (0 if the configuration is not valid).
    static std::size_t storage_bytes(const DfeReceiverConfig& config) noexcept;
    // T/2 samples kept for a configuration (a power of two).
    static std::size_t ring_size_for(InterleaverKind max_interleaver) noexcept;
    // Info bits of the longest block accepted: what the caller's Viterbi and SISO decoders must hold.
    static std::size_t max_input_bits_for(InterleaverKind max_interleaver) noexcept;
    // Data symbols in the longest frame (the chain BCJR's block): 256.
    static constexpr std::size_t max_frame_symbols = 256;

    // Sets up in `storage` (at least storage_bytes(), kept in place and used by nothing else) with
    // the caller's decoders. False, unconfigured, if anything is missing or does not fit.
    bool configure(const DfeReceiverConfig& config, std::span<std::uint8_t> storage,
                   const DfeDecoders& decoders) noexcept;
    bool configured() const noexcept { return ring_ != nullptr; }

    // Back to the start of a stream: front end and ring cleared, the sample count restarts at 0,
    // any burst dropped, counters zero.
    void reset() noexcept;

    // Begins a burst. Call it as soon as the detector reports one, before pushing the samples after
    // the detection: the C# initialises the equaliser at a fixed point after the data start and the
    // reads there depend on how many samples have arrived. False, changing nothing, if not
    // configured, a burst is active, the waveform is 0 or not one this build implements, or the
    // interleaver is longer than configured.
    bool start(const DfeStart& start) noexcept;
    bool active() const noexcept { return active_; }

    // Uses audio (9600 Hz), the same stream the detector was given from the same first sample,
    // and returns how many samples it used: all unless an event was raised (it stops after that
    // sample) or one is waiting (then none).
    std::size_t push_audio(std::span<const float> audio) noexcept;

    // The pending event, if any. A decoded block comes first; take_event() moves to the next.
    DfeEvent event() const noexcept;
    void take_event() noexcept;
    const DfeBlock& block() const noexcept { return block_; }
    const DfeBurstEnd& burst_end() const noexcept { return end_; }

    std::int64_t t2_written() const noexcept { return written_; }
    const DfeReceiverStats& stats() const noexcept { return stats_; }
    const ProbePattern& probe_pattern() const noexcept { return pattern_; }

    // Diagnostic hook, called once per frame (null for none).
    void set_frame_trace(DfeFrameTraceFn fn, void* context) noexcept {
        trace_fn_ = fn;
        trace_context_ = context;
    }

private:
    using ComplexF = dsp::ComplexF;
    static constexpr int max_k = 48;
    static constexpr int max_u = 256;
    static constexpr int superframe_chips = 576;
    static constexpr int max_lags = 25;   // K = 48: base 25
    static constexpr int max_bins = 4 * max_lags;
    static constexpr int max_presence_frames = 400;  // 16 s of the shortest frame (96 symbols)
    static constexpr int max_block_frames = 256;

    void push_t2(ComplexF sample) noexcept;
    void try_track() noexcept;
    bool have_samples_for_chip(double chip) const noexcept;
    ComplexF interpolate(double pos) const noexcept;
    ComplexF read_t2(double half_chips) const noexcept;
    ComplexF read_chip(double chip) const noexcept { return read_t2(2.0 * chip); }
    void fill_window(double symbol_chip, EqSample* window) const noexcept;
    void initialize_dfe() noexcept;
    void process_frame() noexcept;
    double score_probe_patterns(std::int64_t probe_chip, std::int64_t frame_number) noexcept;
    const ComplexF* probe(bool shifted) const noexcept { return shifted ? probe_shifted_.data() : probe_plain_.data(); }
    void start_presence_window() noexcept;
    double probe_presence(std::int64_t probe_chip, const ComplexF* probe, double& plain) noexcept;
    int find_presence_carriers(int lags, double* carriers) const noexcept;
    bool signal_absent(double presence) noexcept;
    void track_probe_timing(std::int64_t probe_chip, const ComplexF* probe) noexcept;
    void retune_carrier(double half_chips_ref, double delta_theta, double delta_omega) noexcept;
    void push_llrs(ComplexF descrambled) noexcept;
    void push_max_log_llrs(ComplexF y, const ComplexF* constellation, int points, const std::uint8_t* labels,
                           int bits, float scale, int scramble) noexcept;
    void add_llr(float llr) noexcept;
    void push_decision(ComplexF wire_symbol) noexcept;
    void finish_block() noexcept;
    void decode_block(const float* llrs, std::uint8_t* info) noexcept;
    void encode_block(const std::uint8_t* info, std::uint8_t* fetched) noexcept;
    bool block_samples_resident() const noexcept;
    void turbo_reequalize(const std::uint8_t* info) noexcept;
    void turbo_reequalize_soft() noexcept;
    void turbo_core(const ComplexF* expected_all, const float* symbol_var, const float* wire_ext, bool allow_pair) noexcept;
    void turbo_frozen_probe_pass() noexcept;
    int frozen_probe_rows(int f, int shift) noexcept;
    bool try_salvage_revert(std::uint8_t* info, std::uint8_t* prev_info) noexcept;
    bool try_salvage_relock(std::uint8_t* info, std::uint8_t* prev_info) noexcept;
    bool run_turbo_loop(std::uint8_t* info, std::uint8_t* prev_info, bool& aborted) noexcept;
    friend class MfbBlockDecoder;
    void end_burst(DfeEndReason reason, std::int64_t payload_bits) noexcept;

    // Configuration.
    DfeReceiverConfig config_{};
    RxFrontEnd front_end_{};
    DfeDecoders decoders_{};

    // Storage carved from the caller's block.
    ComplexF* ring_ = nullptr;
    std::size_t ring_size_ = 0;
    std::span<std::uint8_t> dfe_storage_{};
    float* block_llrs_ = nullptr;
    float* deinterleaved_ = nullptr;
    float* mother_ = nullptr;
    std::uint8_t* info_ = nullptr;
    // Turbo stage (sized for the longest block).
    float* first_pass_llrs_ = nullptr;
    float* turbo_llrs_ = nullptr;
    float* soft_punctured_ = nullptr;
    float* soft_mother_ = nullptr;
    float* soft_mother_post_ = nullptr;
    float* soft_wire_llrs_ = nullptr;
    float* soft_wire_ext_ = nullptr;
    float* soft_var_ = nullptr;
    ComplexF* soft_expected_ = nullptr;
    ComplexF* turbo_expected_ = nullptr;
    std::uint8_t* first_info_ = nullptr;
    std::uint8_t* prev_info_ = nullptr;
    std::uint8_t* mfb_info_ = nullptr;
    std::uint8_t* fetched_ = nullptr;
    std::uint8_t* fetched2_ = nullptr;
    std::uint8_t* coded_ = nullptr;
    std::uint8_t* punctured_bits_ = nullptr;
    std::size_t max_block_symbols_ = 0;
    MfbBlockDecoder mfb_{};
    bool mfb_ready_ = false;  // built for this burst (the C# lazy _mfb)
    std::size_t max_size_bits_ = 0;
    std::size_t max_input_bits_ = 0;
    double* presence_dft_re_ = nullptr;
    double* presence_dft_im_ = nullptr;

    // Front end and ring.
    std::int64_t written_ = 0;

    // The burst.
    bool active_ = false;
    bool tracking_initialized_ = false;
    Mode mode_{};
    InterleaverKind interleaver_kind_ = InterleaverKind::Short;
    int constraint_length_ = 7;
    InterleaverParams il_{};
    Interleaver interleaver_{0, 0};
    const PunctureSpec* puncture_ = nullptr;
    fec::TailBitingViterbiDecoder* viterbi_ = nullptr;
    fec::TailBitingSisoDecoder* siso_ = nullptr;
    const fec::ConvolutionalCode* code_ = nullptr;
    bool frozen_relock_active_ = false;
    double chip0_ = 0;
    double tau_ = 0;
    double omega_ = 0;
    double omega_acquired_ = 0;
    double theta_base_ = 0;
    float agc_gain_ = 1.0f;
    std::int64_t data_start_chip_ = 0;

    // Equaliser and tracking.
    Dfe dfe_{};
    int nff_ = 0;
    int nfb_ = 0;
    int ff_lead_ = 0;
    float init_ridge_ = 0;
    float track_ridge_ = 0;
    std::array<ComplexF, superframe_chips + max_k> known_{};
    std::array<ComplexF, max_k> probe_plain_{};
    std::array<ComplexF, max_k> probe_shifted_{};
    std::array<EqSample, eq_max_taps> decisions_{};  // newest at [0], nfb_ used
    Scrambler scrambler_{};
    std::int64_t frame_chip_ = 0;
    int frame_in_block_ = 0;
    ProbePattern pattern_{};
    int bad_probes_ = 0;
    double probe_mse_ = 0;
    double probe_gain_ref_ = 0;
    double probe_energy_ref_ = 0;
    int collapsed_probes_ = 0;
    bool collapse_armed_ = false;

    // Fading detector.
    double fade_floor_ = 0;
    bool fade_floor_seeded_ = false;
    int frames_since_excursion_ = 0;
    bool fading_ = false;
    bool fading_latched_ = false;

    // Signal-absent release.
    std::array<double, max_presence_frames> presence_ring_{};
    int presence_ring_len_ = 0;
    int presence_frames_ = 0;
    double presence_sum_ = 0;
    double presence_threshold_ = 0;
    std::int64_t presence_probe_chip_ = 0;
    bool presence_probe_shifted_ = false;
    double presence_background_ = 0;
    double presence_frame_seconds_ = 0;
    std::array<double, max_bins> presence_spectrum_{};
    int presence_bins_ = 0;
    int presence_lags_ = 0;
    bool presence_spectrum_seeded_ = false;

    // Block and burst assembly.
    std::size_t block_llr_count_ = 0;
    int block_index_ = 0;
    std::int64_t burst_bits_ = 0;
    std::array<std::uint8_t, 31> eom_tail_{};  // the last (up to) 31 decoded bits, oldest first
    int eom_tail_len_ = 0;
    std::array<std::int64_t, max_block_frames> block_frame_chips_{};
    int block_frames_ = 0;

    // Scratch, kept in the object so nothing is on a small stack.
    std::array<EqSample, eq_max_taps> window_{};
    std::array<EqSample, eq_max_taps> past_{};
    std::array<EqSample, eq_max_taps> past2_{};
    std::array<EqSample, eq_max_taps> start_taps_{};
    std::array<EqSample, eq_max_taps> end_taps_{};
    std::array<EqSample, eq_max_taps> traj_end_{};
    std::array<EqSample, eq_max_taps> taps_prev_{};
    std::array<EqSample, eq_max_taps> taps_cur_{};
    std::array<EqSample, eq_max_taps> frame_start_decisions_{};
    std::array<ComplexF, max_u> pass1_{};
    std::array<ComplexF, max_u> pass2_{};
    // Turbo per-frame scratch.
    std::array<ComplexF, max_u> rotors_{};
    std::array<int, max_u> nibbles_{};
    std::array<float, max_u> x2_{};
    std::array<ComplexF, max_u> rx_wire_{};
    std::array<ComplexF, max_u> rx_alt_{};
    std::array<ComplexF, max_u> rx_desc_{};
    std::array<ComplexF, max_u> h1_span_{};
    std::array<ComplexF, max_u> h2_span_{};
    std::array<float, max_u> nv_span_{};
    std::array<float, max_u> past_var_{};
    std::array<float, 4 * max_u> frame_llrs_{};
    std::array<float, 16 * max_u> log_priors_{};
    std::array<ComplexF, max_k> preceding_{};
    std::array<EqSample, eq_max_taps> turbo_saved_taps_{};
    std::array<EqSample, eq_max_taps> causal_taps_{};
    std::array<ComplexF, 2 * max_k> probe_y_{};
    std::array<int, 2 * max_k> probe_idx_{};
    // The same as the chain BCJR takes them (std::complex<float>; the values are copied, not rounded).
    std::array<std::complex<float>, max_u> bcjr_rx_{};
    std::array<std::complex<float>, max_u> bcjr_h1_{};
    std::array<std::complex<float>, max_u> bcjr_h2_{};
    std::array<float, 4 * max_u> bcjr_llrs_{};
    std::array<tables::Complex32, max_k> bcjr_preceding_{};

    // Events.
    bool pending_block_ = false;
    bool pending_end_ = false;
    DfeBlock block_{};
    DfeBurstEnd end_{};
    DfeReceiverStats stats_{};
    DfeFrameTrace trace_{};
    DfeFrameTraceFn trace_fn_ = nullptr;
    void* trace_context_ = nullptr;
};

}  // inline namespace

}  // namespace m0lte::ms110d
