// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The one receive front end and sample ring every receive stage reads: 9600 Hz audio mixed down by
// the 1800 Hz carrier, the root-raised-cosine matched filter, every second output kept (the 4800 Hz
// "T/2" stream, two samples per symbol) and stored in a ring. It is the C# Ms110dDemodulator's
// Process and the ring write of PushT2, which the C# also has exactly once: the preamble detector,
// the WN 0 receiver and the equaliser path all read the same samples (issue 27). T/2 sample p is
// made from audio sample 2 p + 1.
//
// The ring is caller storage (a power of two, at least the longest look-back of every reader).
// Whoever drives the stream pushes audio a sample at a time and, each time push() returns true,
// tells the readers a new T/2 sample has arrived. Readers never write to it.
//
// Internal (src/ is not installed). No allocation, no exceptions.

#include "dsp/complex_f.hpp"
#include "rx_front_end.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

class RxStream {
public:
    // Designs the filter for the roll-off and takes the ring (a power of two, at least 4096 samples,
    // kept in place). False, unconfigured, if either is not valid.
    bool configure(double roll_off, std::span<dsp::ComplexF> ring) noexcept {
        ring_ = nullptr;
        if (ring.size() < 4096 || (ring.size() & (ring.size() - 1)) != 0) return false;
        if (!front_end_.configure(roll_off)) return false;
        ring_ = ring.data();
        mask_ = ring.size() - 1;
        reset();
        return true;
    }

    bool configured() const noexcept { return ring_ != nullptr; }

    // Back to the start of a stream: filter and ring cleared, both counts zero.
    void reset() noexcept {
        if (ring_ == nullptr) return;
        front_end_.reset();
        for (std::size_t i = 0; i <= mask_; ++i) ring_[i] = {};
        written_ = 0;
        used_ = 0;
    }

    // Takes one audio sample; true when it made a new T/2 sample (every second one), which is then
    // at(written() - 1).
    bool push(float sample) noexcept {
        ++used_;
        dsp::ComplexF t2;
        if (!front_end_.push(sample, t2)) return false;
        ring_[static_cast<std::size_t>(written_) & mask_] = t2;
        ++written_;
        return true;
    }

    // Audio samples taken and T/2 samples made since configure() or reset().
    std::int64_t used() const noexcept { return used_; }
    std::int64_t written() const noexcept { return written_; }

    std::size_t size() const noexcept { return ring_ == nullptr ? 0 : mask_ + 1; }

    // T/2 sample `index`, as stored (the caller checks it is still held, see held()).
    const dsp::ComplexF& at(std::int64_t index) const noexcept {
        return ring_[static_cast<std::size_t>(index) & mask_];
    }

    // True if sample `index` has been made and not yet overwritten.
    bool held(std::int64_t index) const noexcept {
        return index >= 0 && index < written_ && written_ - index <= static_cast<std::int64_t>(mask_ + 1);
    }

private:
    RxFrontEnd front_end_{};
    dsp::ComplexF* ring_ = nullptr;
    std::size_t mask_ = 0;
    std::int64_t written_ = 0;
    std::int64_t used_ = 0;
};

// A stream with its ring inside, for a stage used on its own (tests and tools). N is a power of two.
template <std::size_t N>
class OwnedRxStream : public RxStream {
public:
    bool configure(double roll_off) noexcept { return RxStream::configure(roll_off, ring_storage_); }

private:
    std::array<dsp::ComplexF, N> ring_storage_{};
};

}  // namespace m0lte::ms110d
