// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The shared noise generator of tools/csharp-vectors (Wn0Dump.SharedNoise): SplitMix64 uniforms
// and Box-Muller pairs (cos then sin) in double, so a C++ test can add exactly the noise the C#
// tool added before it ran the C# receiver. Test support only.

#include <cmath>
#include <cstdint>

namespace ms110d_test {

class SharedNoise {
public:
    explicit SharedNoise(std::uint64_t seed) : state_(seed) {}
    double next() {
        if (have_spare_) {
            have_spare_ = false;
            return spare_;
        }
        const double r = std::sqrt(-2.0 * std::log(uniform()));
        const double a = 2.0 * 3.14159265358979323846 * uniform();
        spare_ = r * std::sin(a);
        have_spare_ = true;
        return r * std::cos(a);
    }

private:
    std::uint64_t next_u64() {
        std::uint64_t z = state_ += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform() { return (static_cast<double>(next_u64() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    std::uint64_t state_;
    double spare_ = 0;
    bool have_spare_ = false;
};

}  // namespace ms110d_test
