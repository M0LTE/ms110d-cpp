// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Test signals for the preamble detector, built identically here and in the C# tool
// (tools/csharp-vectors/DetectDump.cs): the same seeded noise, the same carrier offset and
// fractional delay, so both detectors can be given the same samples and their answers compared
// from the committed dump (tests/data/csharp-detect-0.4.1.txt). docs/dev/preamble-detector.md
// has the specification. Test support only.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ms110d_test {

// splitmix64 with 53 bit uniforms and a unit variance "Gaussian" made of twelve uniforms less six,
// which needs only exact double additions, so the C# and the C++ produce the same stream.
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next();
    double uniform();
    double gauss();

private:
    std::uint64_t state_;
};

struct Scenario {
    std::string case_name;
    double snr_db = 0;       // SNR in 3 kHz; NaN for no noise
    double df_hz = 0;        // carrier offset
    double delay = 0;        // delay in input samples, fractional allowed
    int lead = 0;            // samples of lead-in (noise only)
    std::uint64_t seed = 0;
    double threshold = 0.32;
    double gain = 1.0;       // signal scale (a weak signal), applied before the noise is sized
};

// A reference case's baseband, delayed, put on a carrier 1800 + df_hz, with a lead-in and seeded
// noise: the C# Impair.Build. Needs the reference files in `ref_dir`.
std::vector<float> build_scenario(const std::filesystem::path& ref_dir, const Scenario& s);

// A noise-only stream of `samples` samples with standard deviation sigma.
std::vector<float> noise_only(std::uint64_t seed, int samples, double sigma);

// One line of the C# dump: "kind name key=value ... -> key=value ...".
struct DumpLine {
    std::string kind;                                // ref, scn or noise
    std::string name;                                // the case name for ref and scn
    std::map<std::string, std::string> params;       // before the arrow
    std::map<std::string, std::string> result;       // after the arrow

    double real(const std::map<std::string, std::string>& m, const std::string& key) const;
    long long integer(const std::map<std::string, std::string>& m, const std::string& key) const;
};

std::vector<DumpLine> load_dump(const std::filesystem::path& file);  // throws std::runtime_error

Scenario scenario_of(const DumpLine& line);

}  // namespace ms110d_test
