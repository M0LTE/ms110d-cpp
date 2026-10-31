// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The receive path: 9600 Hz audio in, decoded bursts out, for MIL-STD-188-110D Appendix D 3 kHz
// bursts of every waveform (WN 0 to 8 and 13) and interleaver. It is the C# Ms110dDemodulator,
// with the same options and the same defaults, and in the default (double) build it decodes the
// same bits as the C# 0.4.1 receiver to the bit: preamble search and identification, the WN 0
// Walsh receiver, the equaliser with its turbo, salvage and MFB stages, the soft decoders, the EOM
// scan and every end of burst.
//
// Events. Where the C# raises events, the receiver queues them and the caller polls:
//   - BurstStarted    a preamble was identified (the C# Lock becomes set): waveform, interleaver,
//                     constraint length, carrier offset;
//   - BlockDecoded    one interleaver block decoded: its index and its info bits (the C#
//                     BlockDecoded);
//   - BurstCompleted  the burst ended (the C# BurstCompleted): why, how many blocks, and the
//                     payload length. The payload is the burst's block bits in order, cut to
//                     payload_bits (before the EOM, or all of them without one); the bits are not
//                     repeated in this event;
//   - FirstPassSoft, TurboSoft  per block soft values, wire order, positive for bit 0 (the C#
//                     FirstPassBlockLlrs and TurboBlockLlrs), only when RxOptions::soft_decisions;
//   - BurstRefused    a preamble was identified but its interleaver is longer than
//                     RxOptions::max_interleaver, so the burst is not received (no C# counterpart:
//                     the C# accepts every interleaver).
// Every event carries `sample`, the number of input samples the receiver had taken when it was
// raised, and the burst's waveform, interleaver, constraint length and carrier offset.
//
// Use:
//     RxOptions options;                                    // the C# Ms110dDemodOptions defaults
//     size_t bytes;  Receiver::storage_bytes(options, bytes);
//     std::vector<uint8_t> memory(bytes);
//     Receiver rx;  rx.configure(options, memory);
//     for each chunk of audio (any size):
//         size_t done = 0;
//         while (done < chunk.size()) {
//             size_t used;  rx.process(chunk.subspan(done), used);  done += used;
//             for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) { ... }
//         }
// process() stops right after the sample that raised events, so the events never pile up; until
// they are all taken it takes no more audio (Status::State). Drain after every process() call, also
// when it consumed everything: the last sample may have raised events. The spans in an event point into the
// receiver and stay valid until the next process() or reset().
//
// Memory. Nothing is allocated, ever, and nothing throws. All working memory is the one block given
// to configure(), sized by storage_bytes() from the options (mostly from the longest interleaver
// accepted, RxOptions::max_interleaver: from 1.4 MiB for the ultra-short interleaver to 58.2 MiB for
// the long; docs/dev/receiver.md has the table). Not thread safe: use one
// object from one thread at a time.

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/mode.hpp>
#include <m0lte/ms110d/status.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace m0lte::ms110d {

// Why a burst ended (the C# Ms110dBurstEndReason, the D.5.4.5 exits; same order).
enum class BurstEndReason : std::int32_t {
    Eom = 0,                 // the EOM was found in the decoded bits
    MaxInputDataBlocks = 1,  // RxOptions::max_input_data_blocks reached
    Terminated = 2,          // terminate()
    SignalLost = 3,          // the signal disappeared mid-burst
    SignalAbsent = 4,        // the mini-probes were indistinguishable from noise for 16 s (not WN 0)
    NewPreamble = 5,         // a new preamble was found while this burst was locked, so it was let go of
};

// The C# TurboNsegMode: how the turbo stage prices the noise per segment of a frame.
enum class TurboNoisePricing : std::int32_t {
    SpikeUp = 0,  // the shipped default (the C# null or "spikeup")
    Off = 1,      // "off": the frame's constant floor throughout
    Spike2s = 2,  // "spike2s": the two-sided measurement variant
};

// The C# Ms110dRxState.
enum class RxState : std::int32_t {
    Searching = 0,        // looking for a preamble
    ReadingPreamble = 1,  // a peak was found; reading the downcount and waveform ID
    Tracking = 2,         // a burst is identified (BurstStarted) and being demodulated
};

// Receiver options: the C# Ms110dDemodOptions with the same defaults, then two of this library's
// own, which size the memory.
struct RxOptions {
    // Stop after this many input-data blocks; 0 is no limit (D.5.4.5.3).
    int max_input_data_blocks = 0;
    // Normalised matched-filter threshold for preamble detection.
    double sync_threshold = 0.32;
    // RLS forgetting factor; unset keeps the frame-tied 1 - ln 10 / U.
    std::optional<float> rls_forgetting_factor;
    // Skip the turbo re-equalisation (and the MFB decoder), decoding from the first pass alone.
    bool disable_turbo = false;
    // Anchored-solve ridge for every probe; unset keeps the per probe length values.
    std::optional<float> track_ridge;
    // The turbo stage's per-segment noise pricing.
    TurboNoisePricing turbo_noise_pricing = TurboNoisePricing::SpikeUp;
    // Roll-off of the receive matched filter, 0.05 to 1 (0.35 is the D.5.1.5 value).
    double roll_off = 0.35;
    // Never end a burst as SignalAbsent (the statistic is still computed).
    bool presence_release_off = false;

    // Not in the C#: the longest interleaver accepted, for every waveform. Memory is sized for it; a
    // burst with a longer one is not received (a BurstRefused event, counted in
    // RxStats::bursts_refused). Long accepts everything, as the C#.
    InterleaverKind max_interleaver = InterleaverKind::Long;
    // Not in the C#: raise the FirstPassSoft and TurboSoft events.
    bool soft_decisions = false;
};

enum class RxEventKind : std::int32_t {
    None = 0,
    BurstStarted = 1,
    BlockDecoded = 2,
    BurstCompleted = 3,
    FirstPassSoft = 4,
    TurboSoft = 5,
    BurstRefused = 6,
};

struct RxEvent {
    RxEventKind kind = RxEventKind::None;
    // Input samples taken when the event was raised (the same count for events raised together).
    std::int64_t sample = 0;

    // The burst, as the preamble announced it (every kind but None; a refused burst too).
    int waveform = -1;
    InterleaverKind interleaver = InterleaverKind::Short;
    int constraint_length = 0;
    // Carrier offset in Hz: after the fit over the matched super-frame on BurstStarted, after the
    // final fit over the last super-frames on the later events (the C# Lock.CfoHz at each point).
    double cfo_hz = 0;

    // BlockDecoded and the soft events: block number within the burst, from 0.
    int block_index = -1;
    // BlockDecoded: the block's info bits, one per byte (0 or 1).
    std::span<const std::uint8_t> bits;
    // FirstPassSoft, TurboSoft: one soft value per interleaver bit, wire (fetched) order.
    std::span<const float> soft;

    // BurstCompleted.
    BurstEndReason reason = BurstEndReason::Eom;
    int blocks = 0;                // input-data blocks decoded
    std::int64_t payload_bits = 0; // bits before the EOM, or every decoded bit without one
};

// Counts since configure() or reset(): the C#'s public counters, and one of this library's.
struct RxStats {
    double peak_search_metric = 0;  // highest matched-filter metric seen
    std::int64_t turbo_converged = 0;
    std::int64_t turbo_reverted = 0;
    std::int64_t turbo_salvaged = 0;
    std::int64_t turbo_aborted = 0;
    std::int64_t turbo_skipped = 0;
    std::int64_t collapse_resolves = 0;
    std::int64_t signal_absent_releases = 0;
    std::int64_t mfb_offered = 0;
    std::int64_t mfb_selected = 0;
    std::int64_t agc_resolves = 0;    // bursts whose input level gain was not 1 (the C# AgcResolves)
    std::int64_t bursts_refused = 0;  // identified, but longer than RxOptions::max_interleaver
    std::int64_t samples_taken = 0;   // input samples taken
};

// Sample rate of the audio the receiver takes.
inline constexpr int rx_sample_rate_hz = 9600;

// The longest event payloads over every mode: info bits of one block (BlockDecoded) and soft values
// of one block (the soft events), both WN 8 with the long interleaver. Receiver::max_block_bits()
// and max_soft_values() give the longest for the interleavers a receiver accepts.
inline constexpr std::size_t rx_max_block_bits = 49152;
inline constexpr std::size_t rx_max_soft_values = 65536;

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

class Receiver {
public:
    Receiver() noexcept = default;
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // Moving hands the receiver, with its memory block, to the new object and leaves the old one
    // unconfigured.
    Receiver(Receiver&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
    Receiver& operator=(Receiver&& other) noexcept {
        if (this != &other) {
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    // Bytes of storage configure() needs for these options. InvalidArgument for an option out of
    // range (a negative block limit, a roll-off outside 0.05 to 1, a non-finite threshold, factor or
    // ridge, an unknown interleaver or pricing value).
    static Status storage_bytes(const RxOptions& options, std::size_t& bytes) noexcept;

    // Validates the options and builds the receiver in `storage` (at least storage_bytes(), any
    // alignment, kept in place and used by nothing else while this object is). BufferTooSmall if it
    // is smaller. On failure the receiver is left as it was. Starts searching.
    Status configure(const RxOptions& options, std::span<std::uint8_t> storage) noexcept;
    bool configured() const noexcept { return impl_ != nullptr; }

    // Takes audio (9600 Hz, any number of samples) and sets `consumed` to how many it used: all of
    // them, unless a sample raised events, when it stops right after that sample. With events still
    // queued it takes none and returns State; State too if not configured.
    Status process(std::span<const float> audio, std::size_t& consumed) noexcept;

    // The next queued event, taken off the queue, or one of kind None when there are none.
    RxEvent next_event() noexcept;
    // The same event without taking it.
    RxEvent peek_event() const noexcept;

    // The terminate-receive command (D.5.4.5.2, the C# Terminate): on the next sample a burst in
    // progress ends (BurstCompleted, Terminated) and the receiver returns to the search.
    void terminate() noexcept;

    // Back to the start of a stream (the C# Reset): front end cleared, any burst and any queued
    // event dropped, counters zero, sample count zero.
    void reset() noexcept;

    RxState state() const noexcept;
    // True from an accepted preamble peak until the burst ends (the C# CarrierDetect).
    bool carrier_detect() const noexcept { return state() != RxState::Searching; }
    RxStats stats() const noexcept;
    // Input samples taken since configure() or reset().
    std::int64_t samples_taken() const noexcept;
    // The longest BlockDecoded bits and soft event values this receiver can raise (0 if not
    // configured): what a caller's buffers need.
    std::size_t max_block_bits() const noexcept;
    std::size_t max_soft_values() const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // inline namespace

}  // namespace m0lte::ms110d
