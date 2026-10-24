// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The equaliser test scenarios: a deterministic generator of received samples (a burst of known
// symbols through a T/2 multipath channel, a slow rotation and noise) and a script that drives the
// Dfe the way the C# demodulator's frame loop does. It is the twin of tools/csharp-vectors/
// DfeDump.cs, which runs the same generator and script on the C# Dfe; keep them in step. The
// generator uses only integer arithmetic and IEEE add, multiply and square root in double, so the
// two languages make identical samples (each scenario records hashes to prove it).

#include "equaliser.hpp"

#include <complex>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ms110d_test {

using Sample = m0lte::ms110d::EqSample;

struct EqScenario {
    std::string name;
    std::string source = "synthetic";  // "synthetic" or a reference case directory name
    int k = 32, u = 96, frames = 8, ff = 24, fb = 12, lead = 13;
    float init_ridge = 1e-3f, track_ridge = 0.15f, lambda = 0.976f, mu = 0.05f;
    double noise_var = 0.05;
    std::uint64_t seed = 1;
    int chan = 0;
    std::string pass = "static";  // static (3 pass), fading (trajectory), nlms
    bool extras = false;          // nested training and soft-label rows
    bool quiet = false;           // only count symbol errors
    int train_len = 192;
};

struct EqProbe {
    bool solved = false;
    Sample corr, post;
    double mse = 0;
    float ffe = 0;
};

struct EqResult {
    std::uint64_t txhash = 0, xhash = 0;
    bool init_solved = false;
    double init_mse = 0;
    float init_ffe = 0;
    bool nested_seen = false, nested_solved = false;
    float nested_ffe = 0;
    std::vector<EqProbe> probes;
    std::vector<std::vector<Sample>> y;  // per frame, the equalised (descrambled) symbols
    std::vector<std::string> d;          // per frame, the decisions as +/-
    std::vector<Sample> taps;
    long errors = 0, counted = 0;
};

// Runs a scenario; ref_dir is the reference-file directory, needed only for a source other than
// "synthetic" (an empty result with txhash 0 comes back if the files are missing).
EqResult run_eq_scenario(const EqScenario& s, const std::filesystem::path& ref_dir);

struct TirScenario {
    std::string name;
    int chan = 0;
    double noise_var = 0.1;
    bool allow_pair = true;
    bool use_only_lag = false;
};

struct TirResult {
    std::uint64_t xhash = 0;
    struct Pass {
        m0lte::ms110d::TirSolve t;
        float ffe = 0;
    };
    std::vector<Pass> passes;
    std::vector<Sample> y;
};

TirResult run_tir_scenario(const TirScenario& s);

// Number of T/2 channel taps defined (chan index range).
inline constexpr int eq_channel_count = 4;

// Memory the equaliser block needs for a tap pair, in a std::vector, for convenience.
std::vector<std::uint8_t> eq_storage(int ff, int fb);

}  // namespace ms110d_test
