// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The MIL-STD-188-110D performance masks, stand-alone: Table D-LXIV (AWGN and Poor channel, 3 kHz) and the
// D-LXV three path static channel for WN 2, run on audio made by the C# reference through M0LTE.HfChannel
// (tools/csharp-vectors, masks mode; scripts/generate-masks.sh; never committed). Each point has bursts of
// random payload at the mask SNR, the C# decoder's error count on exactly that audio, and here the C++
// receiver's. Two checks per point (docs/dev/masks.md):
//   (a) the C++ meets the mask wherever the C# does: the C# repo's accept rule (BER at most 1e-5; with fewer than
//       30 errors, the 97.5 % Poisson upper bound at most 1e-5) holds for the C++ whenever it holds for the C#;
//   (b) the C++ error count is within statistical noise of the C#'s on the same audio.
// The result lines go to stdout and, when MS110D_MASK_LOG is set, one CSV row per point is appended to that file.

#include "mask_driver.hpp"
#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double target_ber = 1e-5;

// 97.5 % upper confidence bound on a Poisson mean given k events (the C# test's Wilson-Hilferty form).
double poisson_upper_975(std::int64_t k) {
    const double nu = (2.0 * static_cast<double>(k)) + 2.0;
    const double z = 1.959964;
    const double h = 2.0 / (9.0 * nu);
    return nu * std::pow(1 - h + (z * std::sqrt(h)), 3) / 2.0;
}

// The C# AssertMask rule: every burst acquired, and BER <= 1e-5 directly with 30 or more errors, else
// the Poisson bound on the count over the bits.
bool meets_mask(std::int64_t errors, std::int64_t bits, int acquisition_failures) {
    if (acquisition_failures != 0) return false;
    if (errors >= 30) return static_cast<double>(errors) / static_cast<double>(bits) <= target_ber;
    return poisson_upper_975(errors) / static_cast<double>(bits) <= target_ber;
}

struct Burst {
    int index = 0;
    std::int64_t audio_offset = 0, samples = 0, bits_offset = 0;
    std::int64_t cs_decoded = 0, cs_errors = 0;
    std::string cs_end;
};

struct Point {
    std::string id, channel;
    int wn = 0;
    double snr_db = 0;
    std::int64_t payload_bits = 0;
    double scale = 4096;
    std::vector<Burst> bursts;
    std::int64_t cs_acquisition_failures = 0;
};

std::string value_after(const std::vector<std::string>& t, const std::string& key) {
    for (std::size_t i = 0; i + 1 < t.size(); ++i)
        if (t[i] == key) return t[i + 1];
    FAIL("manifest: no " << key);
    return {};
}

Point read_point(const std::string& path) {
    std::ifstream f(path);
    Point p;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::vector<std::string> t;
        for (std::string w; ss >> w;) t.push_back(w);
        if (t[0] == "point") {
            p.id = t[1];
            p.channel = value_after(t, "channel");
            p.wn = std::atoi(value_after(t, "wn").c_str());
            p.snr_db = std::atof(value_after(t, "snr_db").c_str());
            p.payload_bits = std::atoll(value_after(t, "payload_bits").c_str());
            p.scale = std::atof(value_after(t, "scale").c_str());
        } else if (t[0] == "burst") {
            Burst b;
            b.index = std::atoi(t[1].c_str());
            b.audio_offset = std::atoll(value_after(t, "audio_offset").c_str());
            b.samples = std::atoll(value_after(t, "samples").c_str());
            b.bits_offset = std::atoll(value_after(t, "bits_offset").c_str());
            b.cs_decoded = std::atoll(value_after(t, "cs_decoded").c_str());
            b.cs_errors = std::atoll(value_after(t, "cs_errors").c_str());
            b.cs_end = value_after(t, "cs_end");
            p.bursts.push_back(b);
            if (b.cs_decoded == 0) ++p.cs_acquisition_failures;
        }
    }
    return p;
}

// The C# error count for a burst: the first payload.Length decoded bits against the payload, the missing ones,
// and any decoded beyond the whole transmitted stream (payload, EOM, fill to a whole block), all errors.
// `tx_bits` is the transmitted stream length; an acquisition failure is every payload bit.
std::int64_t count_errors(const std::vector<std::uint8_t>& decoded, const std::vector<std::uint8_t>& payload,
                          std::int64_t tx_bits) {
    const auto n = static_cast<std::int64_t>(payload.size());
    if (decoded.empty()) return n;
    const auto d = static_cast<std::int64_t>(decoded.size());
    const std::int64_t compared = std::min(d, n);
    std::int64_t errors = n - compared;
    for (std::int64_t i = 0; i < compared; ++i) errors += decoded[static_cast<std::size_t>(i)] != payload[static_cast<std::size_t>(i)];
    return errors + std::max<std::int64_t>(0, d - tx_bits);
}

// The transmitted stream length: payload plus the 32 bit EOM, in whole blocks of the point's input bits. The
// block size is not in the manifest, but the C# decode count of a burst that ended on its EOM is exactly it.
std::int64_t tx_bits_of(const Point& p) {
    for (const Burst& b : p.bursts)
        if (b.cs_end == "Eom" && b.cs_decoded > 0) return b.cs_decoded;
    return p.payload_bits + 32;
}

std::string mask_dir() {
    const char* env = std::getenv("MS110D_MASK_DIR");
    return env != nullptr ? env : MS110D_MASK_DIR;
}

void run_point(const std::string& id) {
    const std::string stem = mask_dir() + "/" + id;
    std::ifstream probe(stem + ".txt");
    if (!probe.good()) {
        if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("the mask files are missing: " << stem);
        SKIP("no mask files at " << stem << " (run scripts/generate-masks.sh)");
    }
    const Point p = read_point(stem + ".txt");
    REQUIRE(p.id == id);
    REQUIRE_FALSE(p.bursts.empty());
    std::ifstream audio_f(stem + ".i16le", std::ios::binary);
    std::ifstream bits_f(stem + ".bits", std::ios::binary);
    REQUIRE(audio_f.good());
    REQUIRE(bits_f.good());

    const std::int64_t tx_bits = tx_bits_of(p);
    ms110d_test::MaskDriver driver;
    std::int64_t bits = 0, cpp_errors = 0, cs_errors = 0, cpp_fail = 0, cs_fail = 0, differing = 0;
    for (const Burst& b : p.bursts) {
        std::vector<std::int16_t> q(static_cast<std::size_t>(b.samples));
        audio_f.clear();
        audio_f.seekg(b.audio_offset);
        audio_f.read(reinterpret_cast<char*>(q.data()), static_cast<std::streamsize>(q.size() * 2));
        REQUIRE(audio_f.good());
        std::vector<float> audio(q.size());
        for (std::size_t i = 0; i < q.size(); ++i) audio[i] = static_cast<float>(q[i] / p.scale);
        std::vector<std::uint8_t> packed((static_cast<std::size_t>(p.payload_bits) + 7) / 8);
        bits_f.clear();
        bits_f.seekg(b.bits_offset);
        bits_f.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
        REQUIRE(bits_f.good());
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(p.payload_bits));
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = (packed[i >> 3] >> (7 - (i & 7))) & 1;

        const ms110d_test::MaskBurst r = driver.run(audio);
        const std::int64_t e = count_errors(r.bits, payload, tx_bits);
        bits += p.payload_bits;
        cpp_errors += e;
        cs_errors += b.cs_errors;
        cpp_fail += r.bits.empty();
        cs_fail += b.cs_decoded == 0;
        differing += e != b.cs_errors;
    }

    const double cpp_ber = static_cast<double>(cpp_errors) / static_cast<double>(bits);
    const double cs_ber = static_cast<double>(cs_errors) / static_cast<double>(bits);
    const bool cs_ok = meets_mask(cs_errors, bits, static_cast<int>(cs_fail));
    const bool cpp_ok = meets_mask(cpp_errors, bits, static_cast<int>(cpp_fail));
#ifdef MS110D_SINGLE_PRECISION
    const char* precision = "single";
#else
    const char* precision = "double";
#endif
    char line[512];
    std::snprintf(line, sizeof line,
                  "[mask] %s %s %s wn %d snr %+g dB: %lld bursts, %lld bits, C# %lld errors (BER %.2e, mask %s), C++ %lld errors "
                  "(BER %.2e, mask %s), %lld bursts differ, acquisition failures C# %lld C++ %lld",
                  precision, id.c_str(), p.channel.c_str(), p.wn, p.snr_db, static_cast<long long>(p.bursts.size()),
                  static_cast<long long>(bits), static_cast<long long>(cs_errors), cs_ber, cs_ok ? "met" : "not shown",
                  static_cast<long long>(cpp_errors), cpp_ber, cpp_ok ? "met" : "not shown", static_cast<long long>(differing),
                  static_cast<long long>(cs_fail), static_cast<long long>(cpp_fail));
    std::printf("%s\n", line);
    if (const char* log = std::getenv("MS110D_MASK_LOG")) {
        // One CSV row per point for scripts/mask-summary.sh.
        std::ofstream(log, std::ios::app)
            << precision << ',' << id << ',' << p.channel << ',' << p.wn << ',' << p.snr_db << ',' << p.bursts.size() << ',' << bits
            << ',' << cs_errors << ',' << cpp_errors << ',' << cs_fail << ',' << cpp_fail << ',' << cs_ok << ',' << cpp_ok << '\n';
    }

    // The controls below the mask are only worth running if the C# makes errors there.
    if (id.rfind("below-", 0) == 0) CHECK(cs_errors > 0);
    // (a) wherever the C# meets the mask on this audio, so does the C++.
    if (cs_ok) CHECK(cpp_ok);
    // (b) within statistical noise of the C#: acquisition failures no more than the C# had, and the error
    // count within three standard deviations of a Poisson count at the larger of the two (at least 2).
    CHECK(cpp_fail <= cs_fail);
    const double larger = static_cast<double>(std::max(cpp_errors, cs_errors));
    const double allowed = std::max(2.0, std::ceil(3.0 * std::sqrt(larger)));
    CHECK(std::fabs(static_cast<double>(cpp_errors - cs_errors)) <= allowed);
}

}  // namespace

#define MASK_POINT(NAME, ID) \
    TEST_CASE("mask " ID, "[mask]") { run_point(ID); }

MASK_POINT(awgn_wn0, "awgn-wn0")
MASK_POINT(awgn_wn1, "awgn-wn1")
MASK_POINT(awgn_wn2, "awgn-wn2")
MASK_POINT(awgn_wn3, "awgn-wn3")
MASK_POINT(awgn_wn4, "awgn-wn4")
MASK_POINT(awgn_wn5, "awgn-wn5")
MASK_POINT(awgn_wn6, "awgn-wn6")
MASK_POINT(awgn_wn7, "awgn-wn7")
MASK_POINT(awgn_wn8, "awgn-wn8")
MASK_POINT(awgn_wn13, "awgn-wn13")
MASK_POINT(poor_wn0, "poor-wn0")
MASK_POINT(poor_wn1, "poor-wn1")
MASK_POINT(poor_wn2, "poor-wn2")
MASK_POINT(poor_wn3, "poor-wn3")
MASK_POINT(poor_wn4, "poor-wn4")
MASK_POINT(poor_wn5, "poor-wn5")
MASK_POINT(poor_wn6, "poor-wn6")
MASK_POINT(poor_wn7, "poor-wn7")
MASK_POINT(poor_wn8, "poor-wn8")
MASK_POINT(poor_wn13, "poor-wn13")
MASK_POINT(static_wn2, "static-wn2")
MASK_POINT(below_awgn_wn2, "below-awgn-wn2")
MASK_POINT(below_awgn_wn6, "below-awgn-wn6")
MASK_POINT(below_poor_wn3, "below-poor-wn3")
MASK_POINT(below_poor_wn5, "below-poor-wn5")
