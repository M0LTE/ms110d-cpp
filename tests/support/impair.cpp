// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "impair.hpp"

#include "reference_files.hpp"

#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace ms110d_test {

std::uint64_t SplitMix::next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double SplitMix::uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

double SplitMix::gauss() {
    double sum = 0;
    for (int i = 0; i < 12; ++i) sum += uniform();
    return sum - 6.0;
}

std::vector<float> noise_only(std::uint64_t seed, int samples, double sigma) {
    SplitMix rng(seed);
    std::vector<float> x(static_cast<std::size_t>(samples));
    for (auto& v : x) v = static_cast<float>(sigma * rng.gauss());
    return x;
}

std::vector<float> build_scenario(const std::filesystem::path& ref_dir, const Scenario& s) {
    constexpr int interp_half = 4;
    const ReferenceCase c(ref_dir / s.case_name, s.case_name);
    const auto bb = c.complex32("baseband_cf32le");
    const long long n = static_cast<long long>(bb.size());
    const int preamble_symbols = static_cast<int>(c.manifest().integer("preamble_symbol_count"));
    const int di = static_cast<int>(std::floor(s.delay));
    const double f = s.delay - di;
    const int total = s.lead + di + (4 * preamble_symbols) + 4000;

    std::vector<double> sig(static_cast<std::size_t>(total));
    for (int k = 0; k < total; ++k) {
        const long long m = static_cast<long long>(k) - s.lead - di;
        double re = 0;
        double im = 0;
        if (f == 0) {
            if (m >= 0 && m < n) {
                re = static_cast<double>(bb[static_cast<std::size_t>(m)].real());
                im = static_cast<double>(bb[static_cast<std::size_t>(m)].imag());
            }
        } else {
            const double pos = static_cast<double>(m) - f;
            const auto i0 = static_cast<long long>(std::floor(pos));
            const double frac = pos - static_cast<double>(i0);
            for (int j = -interp_half + 1; j <= interp_half; ++j) {
                const double u = j - frac;
                const double w = std::abs(u) < 1e-9
                                     ? 1.0
                                     : std::sin(std::numbers::pi * u) / (std::numbers::pi * u) *
                                           (0.5 + (0.5 * std::cos(std::numbers::pi * u / interp_half)));
                const long long idx = i0 + j;
                if (idx >= 0 && idx < n) {
                    re += static_cast<double>(bb[static_cast<std::size_t>(idx)].real()) * w;
                    im += static_cast<double>(bb[static_cast<std::size_t>(idx)].imag()) * w;
                }
            }
        }
        const double phase = 2.0 * std::numbers::pi * (1800.0 + s.df_hz) * k / 9600.0;
        sig[static_cast<std::size_t>(k)] = (re * std::cos(phase)) - (im * std::sin(phase));
    }

    double sigma = 0;
    if (!std::isnan(s.snr_db)) {
        double power = 0;
        for (int k = s.lead; k < total; ++k) power += sig[static_cast<std::size_t>(k)] * sig[static_cast<std::size_t>(k)];
        power /= total - s.lead;
        sigma = std::sqrt(power / std::pow(10.0, s.snr_db / 10.0) * (4800.0 / 3000.0));
    }

    SplitMix rng(s.seed);
    std::vector<float> x(static_cast<std::size_t>(total));
    for (int k = 0; k < total; ++k) {
        x[static_cast<std::size_t>(k)] =
            static_cast<float>(sig[static_cast<std::size_t>(k)] + (sigma > 0 ? sigma * rng.gauss() : 0.0));
    }
    return x;
}

namespace {

void parse_pairs(const std::string& text, std::map<std::string, std::string>& out, std::string* first_plain) {
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) {
            if (first_plain != nullptr && first_plain->empty()) *first_plain = tok;
        } else {
            out[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
    }
}

}  // namespace

double DumpLine::real(const std::map<std::string, std::string>& m, const std::string& key) const {
    const auto it = m.find(key);
    if (it == m.end()) throw std::runtime_error("dump line has no " + key);
    return std::stod(it->second);
}

long long DumpLine::integer(const std::map<std::string, std::string>& m, const std::string& key) const {
    const auto it = m.find(key);
    if (it == m.end()) throw std::runtime_error("dump line has no " + key);
    return std::stoll(it->second);
}

std::vector<DumpLine> load_dump(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<DumpLine> lines;
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty() || text[0] == '#') continue;
        const auto arrow = text.find(" -> ");
        if (arrow == std::string::npos) throw std::runtime_error("bad dump line: " + text);
        DumpLine line;
        std::istringstream head(text.substr(0, arrow));
        head >> line.kind;
        std::string rest;
        std::getline(head, rest);
        if (line.kind != "noise") {
            std::istringstream r(rest);
            r >> line.name;
            std::string remaining;
            std::getline(r, remaining);
            parse_pairs(remaining, line.params, nullptr);
        } else {
            parse_pairs(rest, line.params, nullptr);
        }
        parse_pairs(text.substr(arrow + 4), line.result, nullptr);
        lines.push_back(std::move(line));
    }
    return lines;
}

Scenario scenario_of(const DumpLine& line) {
    Scenario s;
    s.case_name = line.name;
    s.snr_db = line.real(line.params, "snr");
    s.df_hz = line.real(line.params, "df");
    s.delay = line.real(line.params, "delay");
    s.lead = static_cast<int>(line.integer(line.params, "lead"));
    s.seed = static_cast<std::uint64_t>(std::stoull(line.params.at("seed")));
    s.threshold = line.real(line.params, "thr");
    return s;
}

}  // namespace ms110d_test
