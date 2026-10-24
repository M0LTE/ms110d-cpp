// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Preamble search and waveform identification: audio in, "a burst starts here and it is waveform
// W, interleaver I, constraint length K" out. This is the part of the C# Ms110dDemodulator from
// the audio input up to the point where the preamble has told the receiver everything it needs
// to start demodulating; docs/dev/preamble-detector.md names the exact boundary, the precision
// choices and how it is tested against the C#.
//
// Internal for now (not installed, not in the locked public API); the demodulator and the
// poll-and-drain C interface are built on it.
//
// Use: construct (or configure), then process() audio in chunks of any size. process() returns
// how many samples it used; it stops right after the sample that completes a detection, so the
// caller can take_detection() before feeding the rest. take_detection() returns the event once;
// afterwards the detector only runs its front end (the T/2 samples stay readable through
// t2_sample()) until resume_search() sends it back to looking for a preamble.
//
// No allocation after construction and no exceptions. The object is about 90 KB, so keep it
// off a small stack.

#include <m0lte/ms110d/mode.hpp>

#include "dsp/complex_f.hpp"
#include "dsp/fir_filter.hpp"
#include "modulator.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

// What a detection reports. All positions count from the first sample given to process() since
// construction or reset(). "T/2" is the detector's own sample rate, 4800 Hz, two samples per
// symbol: T/2 sample p is the filtered, mixed-down input sample 2p + 1.
struct PreambleDetection {
    // From the waveform ID.
    int waveform = -1;                                 // waveform number, 0 to 8 or 13
    InterleaverKind interleaver = InterleaverKind::Short;
    int constraint_length = 7;                         // 7 or 9
    // The downcount carried by the super-frame the search matched, voted over the following
    // ones: that many more super-frames follow it before the data starts.
    int superframes_after = 0;

    // Where things are. chip0 is chip 0 of the matched super-frame (a position in the filtered
    // T/2 stream, fractional); the first data chip is (superframes_after + 1) * 576 chips later,
    // 2 T/2 samples per chip.
    double chip0_t2 = 0;
    double data_start_t2 = 0;
    // The same data start as a position in the input audio: where the first data symbol's pulse
    // is centred in the received stream, i.e. the filter delay taken off 2 * data_start_t2 + 1.
    // A transmitter's symbol k has its pulse centred at tx sample 4 k + 32 at the default roll-off.
    double data_start_sample = 0;
    // Sub-sample part of the timing: the parabolic fit's correction to the integer T/2 peak.
    double timing_fraction_t2 = 0;
    // Input samples consumed when the waveform ID was decoded (the C# lock) and when the event
    // completed, after the final carrier fit (the event's own position; the same as the lock
    // unless the downcount is above four, when the fit waits for the samples up to the data start).
    std::int64_t locked_at_sample = 0;
    std::int64_t detected_at_sample = 0;

    // Frequency. cfo_hz is the carrier offset after the fine fit over the known preamble chips and
    // then the re-fit over the last (up to four) super-frames before the data, as the C# does it
    // before handing the burst to its Walsh or equaliser stage;
    // coarse_bin_hz is the search grid bin the peak came from (-75 to +75 in 25 Hz steps).
    double cfo_hz = 0;
    double coarse_bin_hz = 0;
    // The carrier model the C# receiver continues with: phase increment per T/2 sample and the
    // phase at chip0, so a reader of the T/2 stream can derotate a position pos by
    // omega * (pos - chip0) + theta_base.
    double carrier_omega = 0;
    double carrier_theta_base = 0;

    // The normalised matched-filter metric at the accepted peak (0 to 1).
    double peak_metric = 0;
};

// Counts since construction or reset(), for characterising false alarms.
struct PreambleSearchStats {
    std::int64_t peaks_accepted = 0;     // matched-filter peaks that went on to read the preamble
    std::int64_t rejected_count = 0;     // downcount parity failed
    std::int64_t rejected_margin = 0;    // waveform ID vote too mushy
    std::int64_t rejected_count_vote = 0;// joint downcount vote too mushy
    std::int64_t rejected_wid = 0;       // waveform ID failed its checksum or names no defined mode
    std::int64_t locks = 0;              // waveform ID decoded (the C# lock), before the final carrier fit
    std::int64_t detections = 0;         // events: the lock plus the final carrier fit
    double peak_search_metric = 0;       // highest metric seen
};

struct PreambleDetectorConfig {
    // Roll-off of the receive matched filter, 0.05 to 1 (the C# Ms110dDemodOptions.RollOff).
    double roll_off = default_roll_off;
    // Normalised matched-filter threshold (the C# SyncThreshold).
    double sync_threshold = 0.32;
};

class PreambleDetector {
public:
    // Sizes: the ring of T/2 samples is the lookback the search needs (the longest wait is five
    // super-frames of 576 chips at 2 samples per chip, plus the walk-back and interpolation).
    static constexpr std::size_t ring_size = 8192;
    static constexpr int chips_per_fixed = 288;      // 9 Walsh symbols of 32 chips
    static constexpr int chips_per_superframe = 576;
    static constexpr int search_bins = 7;            // -75 to +75 Hz in 25 Hz steps

    PreambleDetector() noexcept;

    // Applies a configuration (designs the filter and the matched-filter references) and
    // resets. Returns false, leaving the detector unusable, if the roll-off is not valid.
    bool configure(const PreambleDetectorConfig& config) noexcept;
    bool configured() const noexcept { return taps_ != 0; }

    // Back to the start of a stream: front end cleared, searching, counters zero.
    void reset() noexcept;

    // Uses audio (9600 Hz) and returns how many samples it used, which is all of them unless a
    // detection completed (it stops after that sample) or one is waiting to be taken (then 0).
    std::size_t process(std::span<const float> audio) noexcept;

    bool has_detection() const noexcept { return state_ == State::Detected; }
    // Copies the pending detection into `out` and returns true, or returns false if none. After
    // it the detector does not search until resume_search().
    bool take_detection(PreambleDetection& out) noexcept;
    // Looks for the next preamble from here (the C# BackToSearch: the front end carries on).
    void resume_search() noexcept;

    bool searching() const noexcept { return state_ == State::Searching; }
    const PreambleSearchStats& stats() const noexcept { return stats_; }

    // Input samples used so far, and T/2 samples produced.
    std::int64_t samples_used() const noexcept { return used_; }
    std::int64_t t2_written() const noexcept { return written_; }
    // T/2 sample `index` if it is still in the ring (the last ring_size produced), else zero.
    std::complex<float> t2_sample(std::int64_t index) const noexcept;
    bool t2_available(std::int64_t index) const noexcept {
        return index >= 0 && index < written_ && written_ - index <= static_cast<std::int64_t>(ring_size);
    }

private:
    enum class State { Searching, ReadingPreamble, Detected, Idle };

    using ComplexF = dsp::ComplexF;

    void push_t2(ComplexF sample) noexcept;
    void search_step() noexcept;
    double metric(std::int64_t s, int bin, double energy, ComplexF* segments) const noexcept;
    void accept_peak() noexcept;
    void try_read_preamble() noexcept;
    void back_to_search() noexcept;
    void refine_carrier(int base_chip, const std::uint8_t* known_chips, int chip_count) noexcept;
    static void superframe_chips(int count, int wn, InterleaverKind il, int k, std::uint8_t* out) noexcept;
    void finish_tail() noexcept;
    ComplexF interpolate(double pos) const noexcept;
    ComplexF read_chip(int chip) const noexcept;
    std::uint8_t read_walsh_symbol(int start_chip, const std::array<std::uint8_t, 256>& pn, int pn_offset) const noexcept;
    void accumulate_walsh_mags(int start_chip, const std::array<std::uint8_t, 256>& pn, int pn_offset, double* mags) const noexcept;

    // Front end.
    dsp::FirFilter<max_pulse_taps> filter_re_;
    dsp::FirFilter<max_pulse_taps> filter_im_;
    std::array<ComplexF, 16> mix_table_{};
    std::array<std::array<ComplexF, chips_per_fixed>, search_bins> mf_reference_{};
    std::array<ComplexF, ring_size> ring_{};
    std::size_t taps_ = 0;
    int mix_phase_ = 0;
    bool decimate_toggle_ = false;
    std::array<double, 2> comb_energy_{};
    std::int64_t written_ = 0;
    std::int64_t used_ = 0;

    // Search.
    double sync_threshold_ = 0.32;
    double best_metric_ = 0;
    std::int64_t best_start_ = -1;
    int best_bin_ = 0;
    std::array<double, 256> metric_ring_{};
    std::array<std::uint8_t, 256> metric_bin_ring_{};

    // Reading the preamble.
    State state_ = State::Searching;
    double chip0_ = 0;
    double omega_ = 0;
    double theta_base_ = 0;
    double cfo_hz_ = 0;
    double coarse_bin_hz_ = 0;
    double timing_fraction_ = 0;
    double accepted_metric_ = 0;
    bool count_read_ = false;
    bool tail_pending_ = false;
    int votes_ = 0;

    PreambleDetection event_{};
    PreambleSearchStats stats_{};
};

}  // namespace m0lte::ms110d
