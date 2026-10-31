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
// The detector reads the shared receive front end and ring (rx_stream.hpp), which it is bound to
// at configure(). Whoever drives that stream calls on_t2() after every new T/2 sample; a detector
// used on its own (StandalonePreambleDetector below) drives it itself through process().
//
// Use: configure, then process() audio in chunks of any size. process() returns
// how many samples it used; it stops right after the sample that completes a detection, so the
// caller can take_detection() before feeding the rest. take_detection() returns the event once;
// afterwards the detector only runs its front end (the T/2 samples stay readable through
// t2_sample()) until resume_search() sends it back to looking for a preamble.
//
// No allocation and no exceptions. The object is about 19 KB without the ring (the stand-alone one
// carries its own 128 KB ring), so keep it off a small stack.

#include <m0lte/ms110d/mode.hpp>

#include "dsp/complex_f.hpp"
#include "dsp/fir_filter.hpp"
#include "modulator.hpp"
#include "rx_stream.hpp"

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

    // The carrier before the final re-fit (after the fit on the matched super-frame only), which
    // the equaliser path's input level estimate is read with.
    double cfo_hz_first_fit = 0;
    double carrier_omega_first_fit = 0;
    double carrier_theta_base_first_fit = 0;
    // The C# input level (issue 101 in the C#): the fade-averaged Fixed section correlation
    // amplitude, |sum y conj(k)| / 32 averaged over the Fixed sections of the last up to eight
    // super-frames, read with the first-fit carrier. Reported for every waveform.
    double input_level = 0;
    // The gain the equaliser path applies to every read from here on (the C# _agcGain): 1.0 unless
    // the level is below 0.04, then 0.12 / level up to 32. The C# sets it before the final
    // re-fit and the re-fit here reads with it too, for waveforms 1 and up. The WN0 path does not
    // use a gain, so for waveform 0 this is 1.0 whatever the level.
    float agc_gain = 1.0f;

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
    std::int64_t retries = 0;            // rejected reads read again at the strongest peak (C# issue 8)
    std::int64_t background_candidates = 0;  // peaks read while a burst was being received (C# issue 10)
    std::int64_t new_preambles = 0;      // background reads that passed every gate
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
    // The ring of T/2 samples is the lookback the search needs. The longest is the input level
    // estimate, which reads up to eight super-frames (9216 samples) back from the data start;
    // the search and the votes need under 5900. Interpolation asserts (debug builds) that every
    // read is inside the ring.
    static constexpr std::size_t ring_size = 16384;
    static constexpr int chips_per_fixed = 288;      // 9 Walsh symbols of 32 chips
    static constexpr int chips_per_superframe = 576;
    static constexpr int search_bins = 7;            // -75 to +75 Hz in 25 Hz steps

    PreambleDetector() noexcept = default;
    PreambleDetector(const PreambleDetector&) = delete;
    PreambleDetector& operator=(const PreambleDetector&) = delete;

    // Applies a configuration (the matched-filter references) and binds the detector to `stream`,
    // which must be configured with the same roll-off and hold at least ring_size samples, and
    // stay in place. Resets the detector (not the stream). Returns false, leaving the detector
    // unusable, if the roll-off is not valid or the stream does not fit.
    bool configure(const PreambleDetectorConfig& config, RxStream& stream) noexcept;
    bool configured() const noexcept { return taps_ != 0; }

    // Back to searching with the counters zero. The stream is the driver's to reset.
    void reset() noexcept;

    // For the stream's driver: a new T/2 sample has been pushed. Keeps the running energies and,
    // while searching or reading a preamble, takes the search one step (not if `step` is false:
    // the C# skips the stage on the sample a terminate command ends a burst).
    void on_t2(bool step = true) noexcept;

    // Drives the bound stream itself: pushes audio (9600 Hz) and returns how many samples it used,
    // which is all of them unless a detection completed (it stops after that sample) or one is
    // waiting to be taken (then 0). Only for a detector that is the stream's only reader.
    std::size_t process(std::span<const float> audio) noexcept;

    bool has_detection() const noexcept { return state_ == State::Detected; }
    // Copies the pending detection into `out` and returns true, or returns false if none. After
    // it the detector does not search until resume_search().
    bool take_detection(PreambleDetection& out) noexcept;
    // Looks for the next preamble from here (the C# BackToSearch: the front end carries on).
    void resume_search() noexcept;

    // While a burst is received (after take_detection()) the search runs on in the background from
    // the burst's data start, as the C# does since 0.4.0 (its issue 10). The driver calls
    // background_step() once per T/2 sample after the burst's own stage, while the burst is still
    // on; true means a new preamble passed every gate and the burst should be let go of
    // (BurstEndReason::NewPreamble). When the burst ends, for that or any other reason,
    // end_burst() sends the detector back to the search, or, unless the burst was terminated,
    // straight into the live read of a candidate it was reading in the background, so its
    // first super-frames are not missed.
    bool background_step() noexcept;
    // After background_step() returned true: the interleaver the new preamble announced, and a way
    // to turn it down (the driver would refuse it) and carry on searching in the background.
    InterleaverKind background_interleaver() const noexcept { return read_il_; }
    void reject_background() noexcept {
        if (state_ == State::Background && cand_pending_) reject_candidate();
    }
    void end_burst(bool terminated) noexcept;

    bool searching() const noexcept { return state_ == State::Searching; }
    // True between the lock (the waveform ID decoded, stats().locks counted) and the detection,
    // while the final carrier fit waits for the samples up to the data start; lock_info() then
    // holds the identified parameters and the first-fit carrier.
    bool locked() const noexcept { return state_ == State::ReadingPreamble && tail_pending_; }
    const PreambleDetection& lock_info() const noexcept { return event_; }
    const PreambleSearchStats& stats() const noexcept { return stats_; }

    // Input samples used so far, and T/2 samples produced.
    std::int64_t samples_used() const noexcept { return stream_ == nullptr ? 0 : stream_->used(); }
    std::int64_t t2_written() const noexcept { return stream_ == nullptr ? 0 : stream_->written(); }
    // T/2 sample `index` if it is still in the ring (the last ring_size produced), else zero.
    std::complex<float> t2_sample(std::int64_t index) const noexcept;
    bool t2_available(std::int64_t index) const noexcept {
        return stream_ != nullptr && index >= 0 && index < stream_->written() &&
               stream_->written() - index <= static_cast<std::int64_t>(ring_size);
    }

private:
    enum class State { Searching, ReadingPreamble, Detected, Background };
    enum class Read { NotReady, Rejected, Locked };

    using ComplexF = dsp::ComplexF;

    void search_step() noexcept;
    double metric(std::int64_t s, int bin, double energy, ComplexF* segments) const noexcept;
    void accept_peak() noexcept;
    void begin_candidate(std::int64_t s, int bin) noexcept;
    void load_candidate() noexcept;
    void start_live_read() noexcept;
    void reject_candidate() noexcept;
    void try_read_preamble() noexcept;
    Read read_preamble() noexcept;
    void enter_lock() noexcept;
    void back_to_search() noexcept;
    void refine_carrier(int base_chip, const std::uint8_t* known_chips, int chip_count) noexcept;
    static void superframe_chips(int count, int wn, InterleaverKind il, int k, std::uint8_t* out) noexcept;
    void finish_tail() noexcept;
    double estimate_input_level(std::int64_t data_start_chip) const noexcept;
    ComplexF interpolate(double pos) const noexcept;
    ComplexF read_chip(int chip) const noexcept;
    std::uint8_t read_walsh_symbol(int start_chip, const std::array<std::uint8_t, 256>& pn, int pn_offset) const noexcept;
    void accumulate_walsh_mags(int start_chip, const std::array<std::uint8_t, 256>& pn, int pn_offset, double* mags) const noexcept;

    // Front end (the shared stream) and the matched-filter references.
    RxStream* stream_ = nullptr;
    std::array<std::array<ComplexF, chips_per_fixed>, search_bins> mf_reference_{};
    std::size_t taps_ = 0;
    std::array<double, 2> comb_energy_{};

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
    float agc_gain_ = 1.0f;  // applied to reads in the equaliser path's re-fit
    // What the last successful read announced.
    int read_count_ = 0;
    int read_wn_ = 0;
    InterleaverKind read_il_ = InterleaverKind::Short;
    int read_k_ = 7;

    // The candidate under read: timing and carrier kept apart from the reading state, so one found
    // in the background can be read again live when the burst is let go of.
    double cand_chip0_ = 0;
    double cand_omega_ = 0;
    double cand_cfo_hz_ = 0;
    double cand_coarse_bin_hz_ = 0;
    double cand_fraction_ = 0;
    // C# issue 8: the walkback moves a candidate to the earliest echo. When that read is rejected
    // and the strongest peak was elsewhere, it is read once more there. If that read locks and the
    // earliest candidate was a separate arrival (at least 4 T/2 samples, two chips, earlier), the
    // burst is still tracked from the earliest one, where the equaliser wants its cursor.
    bool alt_available_ = false;
    std::int64_t alt_start_ = 0;
    int alt_bin_ = 0;
    bool track_from_earliest_ = false;
    double earliest_chip0_ = 0;
    double earliest_fraction_ = 0;
    // C# issue 10: the background search starts at the locked burst's data start (a T/2
    // position); cand_pending_ means a candidate it found is being read.
    std::int64_t shadow_from_ = 0;
    bool cand_pending_ = false;

    PreambleDetection event_{};
    PreambleSearchStats stats_{};
};

// A detector with its own front end and ring, for use on its own (tests and tools): the same
// interface, with configure() and reset() covering the stream too.
class StandalonePreambleDetector : public PreambleDetector {
public:
    StandalonePreambleDetector() noexcept { configure(PreambleDetectorConfig{}); }
    bool configure(const PreambleDetectorConfig& config) noexcept {
        stream_.configure(config.roll_off);  // on failure the stream is unconfigured, so the next fails too
        return PreambleDetector::configure(config, stream_);
    }
    void reset() noexcept {
        stream_.reset();
        PreambleDetector::reset();
    }

private:
    OwnedRxStream<PreambleDetector::ring_size> stream_{};
};

}  // namespace m0lte::ms110d
