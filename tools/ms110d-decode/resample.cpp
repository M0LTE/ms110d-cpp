// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "resample.hpp"

#include <cmath>
#include <numeric>

namespace ms110d_tool {

namespace {

constexpr int kTaps = 801;          // odd, so the group delay is a whole number of samples
constexpr double kBeta = 8.0;       // Kaiser window
constexpr double kCutoffHz = 4400.0;
constexpr double kPi = 3.14159265358979323846;

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}

}  // namespace

bool resample_to_9600(const std::vector<double>& in, int rate_in, std::vector<std::int16_t>& out) {
    out.clear();
    if (rate_in < 9600) return false;
    const int g = std::gcd(rate_in, 9600);
    const int up = 9600 / g, down = rate_in / g;

    std::vector<double> h(kTaps);
    if (up == 1 && down == 1) {
        h.assign(kTaps, 0.0);
    } else {
        const double fc = kCutoffHz / (static_cast<double>(rate_in) * up);
        const double half = (kTaps - 1) / 2.0;
        double total = 0;
        for (int i = 0; i < kTaps; ++i) {
            double n = i - half;
            double x = 2 * fc * n;
            double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            double r = n / half;
            double win = bessel_i0(kBeta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(kBeta);
            h[static_cast<size_t>(i)] = 2 * fc * sinc * win;
            total += h[static_cast<size_t>(i)];
        }
        for (double& v : h) v *= up / total;
    }

    auto quantise = [](double v) {
        double r = std::nearbyint(v * 32768.0);  // round half to even, as numpy rint
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        return static_cast<std::int16_t>(r);
    };

    if (up == 1 && down == 1) {
        out.reserve(in.size());
        for (double v : in) out.push_back(quantise(v));
        return true;
    }

    const long long d = (kTaps - 1) / 2;
    const long long nin = static_cast<long long>(in.size());
    const long long nout = nin * up / down;
    out.resize(static_cast<size_t>(nout));
    for (long long m = 0; m < nout; ++m) {
        long long t = static_cast<long long>(down) * m + d;
        long long r = t % up;
        long long base = t / up;  // input index of tap 0
        double acc = 0;
        for (long long j = 0, hi = r; hi < kTaps; ++j, hi += up) {
            long long xi = base - j;
            if (xi < 0) break;
            if (xi < nin) acc += h[static_cast<size_t>(hi)] * in[static_cast<size_t>(xi)];
        }
        out[static_cast<size_t>(m)] = quantise(acc);
    }
    return true;
}

}  // namespace ms110d_tool
