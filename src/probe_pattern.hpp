// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Which mini-probes of a burst are cyclically shifted (D.5.2.2), for a receiver that must accept
// two transmitter conventions without being told which one it hears. The C++ counterpart of the
// C# ProbePattern (M0LTE.Ms110d 0.2.1); issue 68 is the decision behind it.
//
// Data frames are numbered from 0 across the whole burst; g is the frame the probe FOLLOWS (-1 is
// the probe that ends the preamble, which is never shifted).
//
//   current rule (what the C# has always sent): shifted when (g + 2) % F == 0, F the selected
//   interleaver's frame count;
//   standard rule (D.5.2.2): shifted only when g % L == L - 2, L the LONG interleaver's frame
//   count for the waveform, whatever interleaver is selected.
//
// F divides L for every waveform, so the two disagree only at probes the current rule shifts and
// the standard rule does not, and never for the Long interleaver. At a disagreeing probe the
// receiver scores both hypotheses against the received probe (add_evidence) and follows the
// summed evidence: the current rule applies until the standard pattern is clearly proven (the sum
// reaches +switch_evidence), and at -lock_evidence the current rule is confirmed and scoring stops.
//
// Plain integers and one double; no allocation, no exceptions.

#include <cmath>
#include <cstdint>

namespace m0lte::ms110d {

class ProbePattern {
public:
    // Each score is in [-1, 1], so 3 means a few probes agreeing clearly.
    static constexpr double lock_evidence = 3.0;
    // One decisive probe (real probes scored 0.54 to 0.99 for the right hypothesis), never a
    // provisional sign.
    static constexpr double switch_evidence = 0.5;

    // Sets the selected and long interleaver frame counts and forgets any evidence.
    void configure(int frames, int long_frames) noexcept {
        frames_ = frames;
        long_frames_ = long_frames;
        reset();
    }

    // Forgets the evidence (start of a burst).
    void reset() noexcept {
        evidence_ = 0;
        locked_ = false;
        scored_ = 0;
        peak_evidence_ = 0;
    }

    static constexpr bool current_rule(std::int64_t g, int frames) noexcept {
        return g >= 0 && (g + 2) % frames == 0;
    }
    static constexpr bool standard_rule(std::int64_t g, int long_frames) noexcept {
        return g >= 0 && g % long_frames == long_frames - 2;
    }

    bool current(std::int64_t g) const noexcept { return current_rule(g, frames_); }
    bool standard(std::int64_t g) const noexcept { return standard_rule(g, long_frames_); }

    // Whether this probe's two hypotheses still need scoring.
    bool needs_evidence(std::int64_t g) const noexcept { return !locked_ && current(g) != standard(g); }

    // The probe after frame g is shifted, under the pattern the evidence favours.
    bool boundary(std::int64_t g) const noexcept {
        const bool c = current(g);
        const bool s = standard(g);
        return c == s ? c : (evidence_ >= switch_evidence ? s : c);
    }

    // Adds one probe's score: positive when the standard hypothesis fits better.
    void add_evidence(double score) noexcept {
        ++scored_;
        evidence_ += score;
        if (evidence_ > peak_evidence_) peak_evidence_ = evidence_;
        if (std::abs(evidence_) >= lock_evidence) locked_ = true;
    }

    bool locked() const noexcept { return locked_; }
    double evidence() const noexcept { return evidence_; }
    int scored() const noexcept { return scored_; }
    double peak_evidence() const noexcept { return peak_evidence_; }

private:
    int frames_ = 1;
    int long_frames_ = 1;
    double evidence_ = 0;
    bool locked_ = false;
    int scored_ = 0;
    double peak_evidence_ = 0;
};

}  // namespace m0lte::ms110d
