// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The WN 0 (Walsh) receive path, internal like the modulator: audio in, soft channel bits out.
// It is the C# Ms110dDemodulator's WN 0 path (TrackWalsh) with the Wid0WalshModem RAKE
// (DemodulateRake) and the front end that feeds it (mix to baseband, root-raised-cosine
// filter, decimate to the half-chip rate, carrier- and timing-corrected chip reads), ported
// step for step. See docs/dev/wn0-receiver.md for what is and is not here.
//
// What the caller supplies (the C# gets these from acquisition and the carrier re-fit, which
// are other stories): where the burst's chips sit in the audio, the residual carrier (phase
// and frequency), and where the data starts. What comes out is, per interleaver block, one
// max-log soft value per fetched (wire order) bit, positive for bit 0, plus the per-symbol
// Walsh decisions; deinterleaved_block() puts a block into code order and decode_block() runs
// the tail-biting Viterbi decoder (src/fec) on it.
//
// No allocation, no exceptions; every buffer is in the object or given by the caller.

#include <m0lte/ms110d/config.hpp>

#include "fec/tail_biting_viterbi.hpp"
#include "interleaver.hpp"
#include "modulator.hpp"
#include "tables.hpp"
#include "trinomial_scrambler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

// A complex number in the build's precision (the C# Complex32 is float throughout).
struct Cx {
    real re = 0;
    real im = 0;
};

// Where the burst is, in the receiver's own sample counting. The receiver counts half-chip
// (T/2, 4800 Hz) samples from the first audio sample ever pushed after configure() or
// reset(): half-chip sample p is made from audio sample 2 p + 1 (the C# drops the first
// audio sample of each pair, starting with sample 0).
struct Wn0Start {
    // Position (half-chip samples, fractional) of chip 0 of the preamble, as the C# _chip0.
    double chip0 = 0;
    // Slow timing correction, half-chip samples (the C# _tau).
    double tau = 0;
    // Residual carrier: phase increment per half-chip sample, radians (the C# _omega) and the
    // carrier phase at chip0 (_thetaBase). The phase at position pos is
    // theta_base + omega * (pos - chip0).
    double omega = 0;
    double theta_base = 0;
    // Index, counted in chips from chip 0 of the preamble, of the first chip of the first
    // Walsh symbol (the C# _dataStartChip, 576 * (downcount + 1)).
    std::int64_t data_start_chip = 0;
    // The interleaver of the burst (WN 0 Table D-XXXVII row): size in bits, two per symbol.
    Interleaver interleaver{0, 0};
};

// What one Walsh channel symbol gave.
struct Wn0Symbol {
    int dibit = 0;              // the winning candidate (0..3); the first fetched bit is its MSB
    real llr[2] = {0, 0};       // max-log soft values for the two bits, positive means 0
    Cx combined{};              // sum of conj(finger gain) * winner correlation (RAKE output)
    double max_finger_abs = 0;  // largest finger |winner correlation|
    double chip_sum = 0;        // sum of the symbol's own 32 chip magnitudes (the signal-lost yardstick)
    std::uint32_t index = 0;    // symbol number within its interleaver block
};

enum class Wn0Step {
    Inactive,       // start() has not succeeded, or the burst has ended
    NeedMoreAudio,  // the samples this symbol needs are not all here yet
    Symbol,         // one symbol done, block not yet complete
    BlockComplete,  // one symbol done and it was the last of its block: block_llrs is full
    SignalLost,     // the signal-lost discriminator fired (90 weak symbols in a row); idle now. This
                    // symbol was demodulated like any other (last() is valid), as in the C#
};

class Wn0Receiver {
public:
    // RAKE geometry (Wid0WalshModem): 13 fingers at chip delays -6..+6, 44 chips read per symbol.
    static constexpr int chips_per_symbol = 32;
    static constexpr int fingers = 13;
    static constexpr int neg_fingers = 6;
    static constexpr int rake_chips = chips_per_symbol + fingers - 1;
    // Half-chip samples kept (a power of two). A symbol reads 2 * 44 + 8 of them; the caller
    // must keep step()ing between pushes so that the unread part never exceeds this.
    static constexpr std::size_t ring_size = 4096;

    // Designs the receive pulse for the roll-off (must match the transmitter's; 0.35 is the
    // standard's) and clears all state. False, unconfigured, if the roll-off is not valid.
    bool configure(double roll_off) noexcept;
    bool configured() const noexcept { return taps_ != 0; }

    // Clears the front end and drops any burst. The half-chip counter restarts at 0.
    void reset() noexcept;

    // Feeds audio at 9600 Hz. Samples older than ring_size half-chips behind the newest are
    // overwritten (and read as zero): step() often enough that this never matters.
    void push_audio(std::span<const float> audio) noexcept;

    // Half-chip samples made so far.
    std::int64_t half_chips_written() const noexcept { return written_; }

    // Begins a burst. block_llrs is the caller's buffer for one block's soft values, at least
    // interleaver.size_bits() long and kept in place until the burst ends; it is overwritten
    // by each new block. False, changing nothing, if not configured, the interleaver is empty
    // or not valid, the buffer is too small, or the start of the data is already further back
    // than the ring holds.
    bool start(const Wn0Start& start, std::span<real> block_llrs) noexcept;
    bool active() const noexcept { return active_; }

    // Demodulates the next channel symbol if its samples are here (see Wn0Step).
    Wn0Step step() noexcept;

    // step() until something other than Symbol: NeedMoreAudio, BlockComplete or SignalLost.
    Wn0Step poll() noexcept;

    // The symbol the last Symbol or BlockComplete step made.
    const Wn0Symbol& last() const noexcept { return last_; }

    // The block just completed (or being filled), fetched (wire) order, two per symbol.
    std::span<const real> block_llrs() const noexcept { return block_; }

    // Writes the block in code order (the inverse of the transmit interleaver): out[n] is the
    // soft value of coded bit n, ready for the tail-biting decoder (rate 1/2 for WN 0, no
    // puncturing). out must be exactly the block size; false otherwise.
    bool deinterleaved_block(std::span<real> out) const noexcept;

    // Decodes the block just completed: deinterleaves it, converts the soft values to float
    // (the decoder's input type; a copy in the build's precision, into a buffer inside the
    // object, so nothing is allocated) and runs the tail-biting Viterbi decoder, as the C#
    // Ms110dFraming.DecodeBlock does for WN 0 (rate 1/2, so no depuncturing). `decoder` is the
    // caller's, built for the burst's constraint length and at least this block's info bits
    // (size_bits / 2); `info` receives size_bits / 2 hard bits, one per byte. False, writing
    // nothing, if no block is held or `info` or the decoder do not fit.
    bool decode_block(fec::TailBitingViterbiDecoder& decoder, std::span<std::uint8_t> info) noexcept;

    // Magnitude of the current DD gain of finger k (diagnostic), 0 outside 0..fingers-1.
    real gain_magnitude(int k) const noexcept;

    // The carrier model as it stands (the DD loop retunes it every 8 symbols).
    double omega() const noexcept { return omega_; }
    double theta_base() const noexcept { return theta_base_; }

private:
    Cx read_chip(std::int64_t chip) const noexcept;
    Cx interpolate(double pos) const noexcept;
    bool have_samples_for_chip(double chip) const noexcept;

    static constexpr std::size_t ring_mask = ring_size - 1;
    // The longest WN 0 interleaver (Table D-XXXVII, long): 1152 bits.
    static constexpr std::size_t max_block_bits = 1152;

    // Front end.
    std::array<float, max_pulse_taps> pulse_{};
    std::array<real, max_pulse_taps> hist_re_{};
    std::array<real, max_pulse_taps> hist_im_{};
    std::array<Cx, 16> mix_{};
    std::array<Cx, ring_size> ring_{};
    std::size_t taps_ = 0;
    std::size_t hist_pos_ = 0;
    unsigned mix_phase_ = 0;
    bool decimate_toggle_ = false;
    std::int64_t written_ = 0;

    // Walsh demodulator.
    Trinomial159Scrambler scrambler_{};
    std::array<Cx, fingers> gains_{};
    int symbols_seen_ = 0;

    // Burst tracking.
    bool active_ = false;
    double chip0_ = 0;
    double tau_ = 0;
    double omega_ = 0;
    double theta_base_ = 0;
    std::int64_t symbol_chip_ = 0;
    std::uint32_t symbol_in_block_ = 0;
    int weak_symbols_ = 0;
    Cx phase_acc_{};
    int phase_count_ = 0;
    Interleaver interleaver_{0, 0};
    std::span<real> block_{};
    std::array<float, max_block_bits> soft_{};  // deinterleaved block as float, for the decoder
    Wn0Symbol last_{};
};

}  // inline namespace

}  // namespace m0lte::ms110d
