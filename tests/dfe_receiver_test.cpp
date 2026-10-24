// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The equaliser receive path (src/dfe_receiver) against the real C# demodulator: the per-frame
// tracking state, the first-pass soft values, the decoded bits and the end of the burst, from the
// dumps tools/csharp-vectors writes (rx and rx-awgn modes). docs/dev/dfe-receiver.md says what is
// compared and why the tolerances are what they are.

#include "alloc_guard.hpp"
#include "dfe_receiver.hpp"
#include "bcjr.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "probe_pattern.hpp"
#include "puncture.hpp"
#include "framing.hpp"
#include "preamble_detector.hpp"
#include "reference_files.hpp"
#include "rx_driver.hpp"
#include "shared_noise.hpp"

#include <m0lte/ms110d/transmitter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
namespace fs = std::filesystem;
namespace fec = m0lte::fec;

namespace {

std::vector<float> from_base64(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}

std::vector<std::uint8_t> bit_string(const std::string& s) {
    std::vector<std::uint8_t> out;
    for (const char c : s) out.push_back(c == '1' ? 1 : 0);
    return out;
}

std::uint64_t fnv_taps(const DfeFrameTrace& t) {
    std::uint64_t h = 14695981039346656037ULL;
    auto put = [&h](float f) {
        const auto v = std::bit_cast<std::uint32_t>(f);
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    for (int i = 0; i < t.tap_count; ++i) {
        put(t.taps[static_cast<std::size_t>(i)].real());
        put(t.taps[static_cast<std::size_t>(i)].imag());
    }
    return h;
}

double tap_energy(const DfeFrameTrace& t) {
    double e = 0;
    for (int i = 0; i < t.tap_count; ++i) {
        const auto v = t.taps[static_cast<std::size_t>(i)];
        const double re = static_cast<double>(v.real()), im = static_cast<double>(v.imag());
        e += (re * re) + (im * im);
    }
    return e;
}

// "key value key value ..." after a fixed number of leading words.
std::map<std::string, std::string> pairs(std::istringstream& in) {
    std::map<std::string, std::string> m;
    std::string k, v;
    while (in >> k >> v) m[k] = v;
    return m;
}

struct CsFrame {
    long long chip = 0, at = 0;
    std::map<std::string, std::string> kv;
    double d(const std::string& k) const { return std::stod(kv.at(k)); }
    int i(const std::string& k) const { return std::stoi(kv.at(k)); }
};

struct CsBlock {
    long long at = 0;
    std::vector<std::uint8_t> info, turbo;
    std::vector<float> llrs;
};

struct CsCase {
    std::string name;
    int wn = 0;
    std::vector<CsFrame> frames;
    std::vector<CsBlock> blocks;
    std::string reason;
    int end_blocks = 0;
    long long payload = 0, end_at = 0;
};

// A receive dump made by scripts/generate-rx-dumps.sh: skipped when absent, a failure when
// MS110D_REQUIRE_REFERENCE is set (the CI reference job).
void require_dump(const char* path) {
    if (std::ifstream(path).good()) return;
    if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("the receive dump is missing: " << path);
    SKIP("no receive dump at " << path << " (run scripts/generate-rx-dumps.sh)");
}

std::vector<CsCase> load_clean_dump() {
    std::ifstream in(MS110D_CSHARP_RX_DUMP);
    REQUIRE(in.good());
    std::vector<CsCase> cases;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "case") {
            CsCase c;
            std::string k;
            s >> c.name >> k >> c.wn;
            cases.push_back(c);
        } else if (kind == "frame") {
            CsFrame f;
            int index = 0;
            s >> index >> f.chip >> f.at;
            f.kv = pairs(s);
            cases.back().frames.push_back(f);
        } else if (kind == "block") {
            CsBlock b;
            int index = 0;
            std::string info, turbo, llrs;
            s >> index >> b.at >> info >> turbo >> llrs;
            b.info = bit_string(info);
            b.turbo = bit_string(turbo);
            b.llrs = from_base64(llrs);
            cases.back().blocks.push_back(b);
        } else if (kind == "end") {
            CsCase& c = cases.back();
            s >> c.reason >> c.end_blocks >> c.payload >> c.end_at;
        }
    }
    return cases;
}

const char* reason_name(DfeEndReason r) {
    switch (r) {
        case DfeEndReason::Eom: return "Eom";
        case DfeEndReason::MaxInputDataBlocks: return "MaxInputDataBlocks";
        case DfeEndReason::SignalLost: return "SignalLost";
        case DfeEndReason::SignalAbsent: return "SignalAbsent";
    }
    return "?";
}

double rel(double a, double b) { return std::abs(a - b) / (1.0 + std::abs(b)); }

// The worst differences seen, for the summary line.
struct Worst {
    double llr = 0, omega = 0, theta = 0, tau = 0, ref = 0, mse = 0, presence = 0, evidence = 0, taps = 0, floor = 0;
    long tap_hash_equal = 0, tap_hash_total = 0;
};

}  // namespace

TEST_CASE("reference demodulator: every WN 1 to 5 ultra-short and short case against the C# frame by frame",
          "[dfe_receiver]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    require_dump(MS110D_CSHARP_RX_DUMP);
    const std::vector<CsCase> cases = load_clean_dump();
    REQUIRE(cases.size() == 41);
    ms110d_test::RxDriver driver(InterleaverKind::Short, true, false);  // the first pass alone, as the dump
    Worst w;
    for (const CsCase& c : cases) {
        INFO("case " << c.name);
        const ms110d_test::ReferenceCase rc(dir / c.name, c.name);
        std::vector<float> audio = rc.real32("audio_f32le");
        audio.resize(audio.size() + 9600, 0.0f);
        const ms110d_test::RxRun r = driver.run(audio);
        REQUIRE(r.detected);
        CHECK(r.detection.waveform == c.wn);

        // Frames: the same frames at the same samples, the same decisions about fading, collapse
        // and the probe pattern, and the tracking state within the tolerances of the doc.
        REQUIRE(r.frames.size() == c.frames.size());
        for (std::size_t i = 0; i < c.frames.size(); ++i) {
            INFO("frame " << i);
            const DfeFrameTrace& f = r.frames[i];
            const CsFrame& g = c.frames[i];
            CHECK(f.frame_chip == g.chip);
            CHECK(f.at_t2 == g.at);
            CHECK(f.fading == (g.i("fading") == 1));
            CHECK(f.fading_latched == (g.i("latched") == 1));
            CHECK(f.fresh_solve == (g.i("fresh") == 1));
            CHECK(f.bad_probes == g.i("bad"));
            w.omega = std::max(w.omega, rel(f.omega, g.d("omega")));
            w.theta = std::max(w.theta, std::abs(f.theta_base - g.d("theta")));
            w.tau = std::max(w.tau, std::abs(f.tau - g.d("tau")));
            w.ref = std::max({w.ref, rel(f.probe_gain_ref, g.d("gainref")), rel(f.probe_energy_ref, g.d("energyref"))});
            w.mse = std::max(w.mse, rel(f.probe_mse, g.d("probemse")));
            w.floor = std::max(w.floor, rel(f.fade_floor, g.d("floor")));
            w.presence = std::max({w.presence, rel(f.presence, g.d("presence")), rel(f.presence_plain, g.d("plain"))});
            w.evidence = std::max(w.evidence, std::abs(f.pattern_evidence - g.d("evidence")));
            w.taps = std::max(w.taps, rel(tap_energy(f), g.d("tapenergy")));
            // The printed diagnostics (3 or 4 decimals in the C# line).
            CHECK(std::abs(f.probe_gain - g.d("gain")) <= 0.5e-3 + 1e-6);
            CHECK(std::abs(f.pre_mse - g.d("premse")) <= 0.5e-3 + 1e-6);
            CHECK(std::abs(f.tap_change - g.d("tapchange")) <= 0.5e-4 + 1e-6);
            ++w.tap_hash_total;
            std::ostringstream h;
            h << std::hex;
            h.width(16);
            h.fill('0');
            h << fnv_taps(f);
            if (h.str() == g.kv.at("taps")) ++w.tap_hash_equal;
        }

        // Blocks: the decoded bits exactly (the C# with its turbo pass off), the soft values within
        // tolerance, at the same samples.
        REQUIRE(r.blocks.size() == c.blocks.size());
        for (std::size_t b = 0; b < c.blocks.size(); ++b) {
            INFO("block " << b);
            CHECK(r.blocks[b].at_t2 == c.blocks[b].at);
            CHECK(r.blocks[b].info == c.blocks[b].info);
            CHECK(r.blocks[b].info == c.blocks[b].turbo);  // clean: the turbo pass changes nothing
            REQUIRE(r.blocks[b].llrs.size() == c.blocks[b].llrs.size());
            for (std::size_t i = 0; i < c.blocks[b].llrs.size(); ++i) {
                const double a = static_cast<double>(r.blocks[b].llrs[i]);
                const double e = static_cast<double>(c.blocks[b].llrs[i]);
                w.llr = std::max(w.llr, rel(a, e));
            }
        }
        REQUIRE(r.ended);
        CHECK(std::string(reason_name(r.end.reason)) == c.reason);
        CHECK(r.end.blocks == c.end_blocks);
        CHECK(r.end.payload_bits == c.payload);
        CHECK(r.end.at_t2 == c.end_at);

        // And the payload is the transmitted one.
        const std::vector<std::uint8_t> payload = rc.bytes("payload_bits");
        std::vector<std::uint8_t> got;
        for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
        got.resize(static_cast<std::size_t>(r.end.payload_bits));
        CHECK(got == payload);
    }
    std::printf("dfe receiver vs C#, 41 clean cases: soft values %.3g, omega %.3g, theta %.3g rad, tau %.3g, refs %.3g, "
                "probe mse %.3g, fade floor %.3g, presence %.3g, evidence %.3g, tap energy %.3g, tap hashes equal %ld of %ld\n",
                w.llr, w.omega, w.theta, w.tau, w.ref, w.mse, w.floor, w.presence, w.evidence, w.taps, w.tap_hash_equal,
                w.tap_hash_total);
    // Measured here: 0 for everything in the default build (bit for bit, every tap hash equal), and
    // at most 8.3e-7 in the single build. The bounds leave room for another machine's libm, whose
    // last bits of sin and cos move the detector's carrier by about 1e-6 Hz (preamble-detector.md).
    const double soft_bound = 1e-4;
    CHECK(w.llr <= soft_bound);
    CHECK(w.omega <= 1e-8);
    CHECK(w.theta <= 1e-3);
    CHECK(w.tau <= 1e-5);
    CHECK(w.ref <= soft_bound);
    CHECK(w.mse <= soft_bound);
    CHECK(w.presence <= soft_bound);
    CHECK(w.evidence <= soft_bound);
    CHECK(w.taps <= soft_bound);
    CHECK(w.floor <= soft_bound);
}

TEST_CASE("reference demodulator: seeded AWGN and cut bursts on WN 1 to 5 against the C#, decisions exact",
          "[dfe_receiver]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    require_dump(MS110D_CSHARP_RX_AWGN);
    std::ifstream in(MS110D_CSHARP_RX_AWGN);
    REQUIRE(in.good());
    ms110d_test::RxDriver driver(InterleaverKind::Short, true, false);  // the first pass alone, as the dump
    std::vector<float> base;
    int lead_in = 0, lead_out = 0;
    long cut = -1;  // cut trials: the signal stops at this base sample
    double tone_amp = 0, tone_hz = 0;
    double sigma = 0;
    long cut_trials = 0, signal_lost = 0, signal_absent = 0;
    std::string label;
    ms110d_test::RxRun r;
    std::string base_name;
    int block_cursor = 0;
    long trials = 0, blocks = 0, hash_equal = 0, sign_flips = 0, soft_values = 0, decoded_ok = 0;
    double worst_sum = 0, worst_last = 0;
    std::vector<std::uint8_t> payload;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "base" || kind == "cutbase") {
            std::string k1, k2, k3, k4, k5;
            std::size_t n = 0;
            s >> base_name >> n >> k1 >> lead_in >> k2 >> lead_out;
            cut = -1;
            if (kind == "cutbase") s >> k3 >> cut >> k4 >> tone_amp >> k5 >> tone_hz;
            const ms110d_test::ReferenceCase rc(dir / base_name, base_name);
            base = rc.real32("audio_f32le");
            payload = rc.bytes("payload_bits");
            REQUIRE(base.size() == n);
        } else if (kind == "point") {
            std::string k;
            double snr = 0;
            s >> label >> k >> snr >> k >> sigma;
        } else if (kind == "trial") {
            std::string lab, k;
            std::uint64_t seed = 0;
            int wn = 0, frames = 0, nblocks = 0;
            s >> lab >> seed >> k >> wn >> k >> frames >> k >> nblocks;
            ms110d_test::SharedNoise noise(seed);
            std::vector<float> audio(static_cast<std::size_t>(lead_in) + base.size() + static_cast<std::size_t>(lead_out));
            const long trial_index = static_cast<long>(seed % 1000);
            for (std::size_t i = 0; i < audio.size(); ++i) {
                const long kk = static_cast<long>(i) - lead_in;
                const long end = cut >= 0 ? cut : static_cast<long>(base.size());
                const float b = kk >= 0 && kk < end ? base[static_cast<std::size_t>(kk)] : 0.0f;
                // The C# tone: (float)(amp * cos(2 pi f k / 9600)) after the cut, odd trials.
                const float tone = cut >= 0 && trial_index % 2 == 1 && kk >= cut
                                       ? static_cast<float>(tone_amp * std::cos(2.0 * 3.14159265358979323846 * tone_hz *
                                                                                static_cast<double>(kk) / 9600.0))
                                       : 0.0f;
                audio[i] = b + tone + static_cast<float>(sigma * noise.next());
            }
            r = driver.run(audio);
            ++trials;
            if (cut >= 0) ++cut_trials;
            block_cursor = 0;
            INFO(lab << " seed " << seed);
            CHECK(r.frames.size() == static_cast<std::size_t>(frames));
            CHECK(r.blocks.size() == static_cast<std::size_t>(nblocks));
            if (frames > 0) CHECK(r.detection.waveform == wn);
        } else if (kind == "last") {
            const auto kv = pairs(s);
            if (r.frames.empty()) continue;
            const DfeFrameTrace& f = r.frames.back();
            worst_last = std::max({worst_last, rel(f.omega, std::stod(kv.at("omega"))),
                                   std::abs(f.theta_base - std::stod(kv.at("theta"))), std::abs(f.tau - std::stod(kv.at("tau"))),
                                   rel(f.probe_gain_ref, std::stod(kv.at("gainref"))),
                                   std::abs(f.pattern_evidence - std::stod(kv.at("evidence"))),
                                   rel(f.presence, std::stod(kv.at("presence"))), rel(tap_energy(f), std::stod(kv.at("tapenergy")))});
            CHECK(f.fading_latched == (kv.at("latched") == "1"));
        } else if (kind == "block") {
            int index = 0;
            long long at = 0;
            std::string info, turbo, hash, signs;
            double sum_abs = 0;
            s >> index >> at >> info >> turbo >> hash >> signs >> sum_abs;
            INFO("block " << index);
            REQUIRE(block_cursor < static_cast<int>(r.blocks.size()));
            const auto& b = r.blocks[static_cast<std::size_t>(block_cursor++)];
            ++blocks;
            CHECK(b.at_t2 == at);
            CHECK(b.info == bit_string(info));
            // Soft values: hash of the float bits, signs, and the sum of magnitudes.
            std::uint64_t h = 14695981039346656037ULL;
            double sum = 0;
            for (std::size_t i = 0; i < b.llrs.size(); ++i) {
                const auto v = std::bit_cast<std::uint32_t>(b.llrs[i]);
                for (int k = 0; k < 4; ++k) {
                    h ^= (v >> (8 * k)) & 0xFF;
                    h *= 1099511628211ULL;
                }
                sum += std::abs(static_cast<double>(b.llrs[i]));
                const int nib = std::stoi(std::string(1, signs[i / 4]), nullptr, 16);
                const bool neg = ((nib >> (i % 4)) & 1) != 0;
                if (neg != (b.llrs[i] < 0)) ++sign_flips;
                ++soft_values;
            }
            std::ostringstream hs;
            hs << std::hex;
            hs.width(16);
            hs.fill('0');
            hs << h;
            if (hs.str() == hash) ++hash_equal;
            worst_sum = std::max(worst_sum, rel(sum, sum_abs));
        } else if (kind == "end") {
            std::string reason;
            int end_blocks = 0;
            long long payload_bits = 0, end_at = 0;
            s >> reason >> end_blocks >> payload_bits >> end_at;
            if (reason == "None") {
                CHECK_FALSE(r.ended);
            } else {
                REQUIRE(r.ended);
                CHECK(std::string(reason_name(r.end.reason)) == reason);
                CHECK(r.end.blocks == end_blocks);
                CHECK(r.end.payload_bits == payload_bits);
                CHECK(r.end.at_t2 == end_at);
                if (r.end.reason == DfeEndReason::SignalLost) ++signal_lost;
                if (r.end.reason == DfeEndReason::SignalAbsent) ++signal_absent;
            }
            if (r.ended && r.end.reason == DfeEndReason::Eom) {
                std::vector<std::uint8_t> got;
                for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
                got.resize(static_cast<std::size_t>(r.end.payload_bits));
                if (got == payload) ++decoded_ok;
            }
        }
    }
    std::printf("dfe receiver vs C#, AWGN: %ld trials, %ld blocks, soft value hashes equal %ld, sign differences %ld of %ld, "
                "sum |soft| %.3g, last tracking state %.3g; bursts with the payload right %ld; cut bursts %ld, ended signal lost %ld, "
                "signal absent %ld\n",
                trials, blocks, hash_equal, sign_flips, soft_values, worst_sum, worst_last, decoded_ok, cut_trials, signal_lost,
                signal_absent);
    CHECK(trials == 212);
    CHECK(cut_trials == 12);
    CHECK(signal_lost >= 1);   // both exits are reached, and agree with the C# (checked per trial above)
    CHECK(signal_absent >= 1);
    CHECK(sign_flips == 0);
    // Measured here: 0 in the default build, 2.2e-7 and 1.7e-5 in the single build.
    CHECK(worst_sum <= 1e-4);
    CHECK(worst_last <= 1e-3);
}

// ------------------------------------------------------------------ stand-alone (no reference files)

namespace {

struct TxRig {
    std::vector<std::uint8_t> memory;
    Transmitter tx;
    explicit TxRig(const TxSettings& s, std::size_t max_bits) {
        std::size_t bytes = 0;
        REQUIRE(Transmitter::storage_bytes(s, max_bits, bytes) == Status::Ok);
        memory.resize(bytes);
        REQUIRE(tx.configure(s, max_bits, memory) == Status::Ok);
    }
    std::vector<float> burst(const std::vector<std::uint8_t>& payload) {
        std::size_t n = 0;
        REQUIRE(tx.audio_samples(payload.size(), n) == Status::Ok);
        std::vector<float> audio(n);
        std::size_t written = 0;
        REQUIRE(tx.modulate(payload, audio, written) == Status::Ok);
        audio.resize(written);
        return audio;
    }
};

std::vector<std::uint8_t> random_bits(std::mt19937& rng, std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng() & 1u);
    return v;
}

std::vector<std::uint8_t> payload_of(const ms110d_test::RxRun& r) {
    std::vector<std::uint8_t> got;
    for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
    if (r.ended) got.resize(static_cast<std::size_t>(std::max<std::int64_t>(0, r.end.payload_bits)));
    return got;
}

// The C++ transmitter's burst with lead-in, room after it and optional seeded noise at an SNR in
// 3 kHz (the dumps' definition).
std::vector<float> on_air(const std::vector<float>& burst, double snr_db, std::uint64_t seed, int lead_in = 2400) {
    std::vector<float> audio(static_cast<std::size_t>(lead_in) + burst.size() + 9600, 0.0f);
    std::copy(burst.begin(), burst.end(), audio.begin() + lead_in);
    if (!std::isnan(snr_db)) {
        double power = 0;
        for (const float x : burst) power += static_cast<double>(x) * static_cast<double>(x);
        power /= static_cast<double>(burst.size());
        const double sigma = std::sqrt(power / std::pow(10.0, snr_db / 10) * 4800.0 / 3000.0);
        ms110d_test::SharedNoise noise(seed);
        for (float& x : audio) x += static_cast<float>(sigma * noise.next());
    }
    return audio;
}

}  // namespace

TEST_CASE("dfe receiver: the C++ transmitter's bursts decode, every equaliser mode, ultra-short and short, K 7 and 9",
          "[dfe_receiver]") {
    ms110d_test::RxDriver driver(InterleaverKind::Short, false);
    std::mt19937 rng(77);
    int bursts = 0;
    for (const int wn : {1, 2, 3, 4, 5, 6, 7, 8, 13}) {
        for (const InterleaverKind il : {InterleaverKind::UltraShort, InterleaverKind::Short}) {
            for (const int k : {7, 9}) {
                TxSettings s;
                s.waveform_number = wn;
                s.interleaver = il;
                s.constraint_length = k;
                const auto ip = interleaver_3k(wn, il);
                REQUIRE(ip);
                const std::size_t n_in = static_cast<std::size_t>(ip->input_bits);
                TxRig rig(s, 3 * n_in);
                // Block edges: one bit, an EOM ending exactly on a block end, a payload that fills
                // a block (EOM in the next), more than two blocks, and an EOM split one bit over
                // a block boundary.
                // The last: only the EOM's final bit falls in the next block.
                const std::size_t eom_edge = (((31 / n_in) + 1) * n_in) - 31;
                for (const std::size_t bits :
                     {std::size_t{1}, n_in > 32 ? n_in - 32 : n_in + 1, n_in, 2 * n_in + 7, eom_edge}) {
                    INFO("wn " << wn << " il " << static_cast<int>(il) << " k " << k << " bits " << bits);
                    const auto payload = random_bits(rng, bits);
                    const ms110d_test::RxRun r = driver.run(on_air(rig.burst(payload), std::nan(""), 0));
                    REQUIRE(r.detected);
                    CHECK(r.detection.waveform == wn);
                    REQUIRE(r.ended);
                    CHECK(r.end.reason == DfeEndReason::Eom);
                    CHECK(payload_of(r) == payload);
                    CHECK(r.end.blocks == static_cast<int>(framed_block_count(bits, true, n_in)));
                    ++bursts;
                }
            }
        }
    }
    CHECK(bursts == 180);
}

TEST_CASE("dfe receiver: the C++ transmitter's bursts decode with the medium and long interleavers", "[dfe_receiver]") {
    ms110d_test::RxDriver driver(InterleaverKind::Long, false);
    std::mt19937 rng(78);
    int bursts = 0;
    for (const int wn : {1, 2, 3, 4, 5, 6, 7, 8, 13}) {
        for (const InterleaverKind il : {InterleaverKind::Medium, InterleaverKind::Long}) {
            // The long interleaver for a few modes only: a WN 1 long block is ten seconds of audio.
            if (il == InterleaverKind::Long && wn != 3 && wn != 7 && wn != 8) continue;
            TxSettings s;
            s.waveform_number = wn;
            s.interleaver = il;
            const auto ip = interleaver_3k(wn, il);
            REQUIRE(ip);
            const std::size_t n_in = static_cast<std::size_t>(ip->input_bits);
            TxRig rig(s, n_in + 40);
            INFO("wn " << wn << " il " << static_cast<int>(il));
            const auto payload = random_bits(rng, n_in + 3);
            const ms110d_test::RxRun r = driver.run(on_air(rig.burst(payload), std::nan(""), 0));
            REQUIRE(r.ended);
            CHECK(r.end.reason == DfeEndReason::Eom);
            CHECK(payload_of(r) == payload);
            for (const auto& b : r.blocks) CHECK(b.turbo == DfeTurbo::Converged);
            ++bursts;
        }
    }
    CHECK(bursts == 12);
}

TEST_CASE("dfe receiver: the C++ transmitter's bursts decode in noise", "[dfe_receiver]") {
    // WN 1 to 5: 3 dB above the C# dump's upper points, where the C# decoded 18 to 20 of 20 at the
    // point below. The rest: 3 dB above the Table D-LXIV AWGN mask.
    const std::array<std::pair<int, double>, 9> points{
        {{1, -3.0}, {2, 0.0}, {3, 3.0}, {4, 6.0}, {5, 9.0}, {6, 12.0}, {7, 16.0}, {8, 19.0}, {13, 9.0}}};
    ms110d_test::RxDriver driver(InterleaverKind::Short, false);
    std::mt19937 rng(5);
    for (const auto& [wn, snr] : points) {
        TxSettings s;
        s.waveform_number = wn;
        const auto ip = interleaver_3k(wn, InterleaverKind::Short);
        TxRig rig(s, 2 * static_cast<std::size_t>(ip->input_bits));
        int right = 0;
        const int trials = 8;
        for (int t = 0; t < trials; ++t) {
            const auto payload = random_bits(rng, static_cast<std::size_t>(ip->input_bits) + 20);
            const ms110d_test::RxRun r = driver.run(on_air(rig.burst(payload), snr, 900 + static_cast<std::uint64_t>(t)));
            if (r.ended && r.end.reason == DfeEndReason::Eom && payload_of(r) == payload) ++right;
        }
        INFO("wn " << wn << " at " << snr << " dB: " << right << " of " << trials);
        CHECK(right == trials);
    }
}

TEST_CASE("dfe receiver: results do not depend on how the audio is cut up", "[dfe_receiver]") {
    TxSettings s;
    s.waveform_number = 3;
    s.interleaver = InterleaverKind::UltraShort;
    const auto ip = interleaver_3k(3, InterleaverKind::UltraShort);
    TxRig rig(s, 2 * static_cast<std::size_t>(ip->input_bits));
    std::mt19937 rng(9);
    const auto payload = random_bits(rng, static_cast<std::size_t>(ip->input_bits) + 5);
    const auto audio = on_air(rig.burst(payload), 6.0, 31);
    ms110d_test::RxDriver driver(InterleaverKind::Short);
    const ms110d_test::RxRun a = driver.run(audio, 9600);
    REQUIRE(a.ended);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{100000}}) {
        INFO("chunk " << chunk);
        const ms110d_test::RxRun b = driver.run(audio, chunk);
        REQUIRE(b.blocks.size() == a.blocks.size());
        for (std::size_t i = 0; i < a.blocks.size(); ++i) {
            CHECK(b.blocks[i].at_t2 == a.blocks[i].at_t2);
            CHECK(b.blocks[i].llrs == a.blocks[i].llrs);
            CHECK(b.blocks[i].info == a.blocks[i].info);
        }
        REQUIRE(b.frames.size() == a.frames.size());
        for (std::size_t i = 0; i < a.frames.size(); ++i) {
            CHECK(b.frames[i].at_t2 == a.frames[i].at_t2);
            CHECK(b.frames[i].omega == a.frames[i].omega);
            CHECK(b.frames[i].presence == a.frames[i].presence);
        }
        CHECK(b.end.at_t2 == a.end.at_t2);
    }
}

TEST_CASE("dfe receiver: nothing allocates after configure", "[dfe_receiver]") {
    DfeReceiverConfig config;
    config.max_interleaver = InterleaverKind::Short;
    const std::size_t bits = DfeReceiver::max_input_bits_for(config.max_interleaver);
    fec::TailBitingViterbiDecoder k7(fec::k7_133_171, bits);
    fec::TailBitingViterbiDecoder k9(fec::k9_561_753, bits);
    fec::TailBitingSisoDecoder s7(fec::k7_133_171, bits);
    fec::TailBitingSisoDecoder s9(fec::k9_561_753, bits);
    BasicChainBcjr<float> chain(DfeReceiver::max_frame_symbols, 16);
    DfeDecoders dec;
    dec.viterbi_k7 = &k7;
    dec.viterbi_k9 = &k9;
    dec.siso_k7 = &s7;
    dec.siso_k9 = &s9;
    dec.chain = &chain;
    std::vector<std::uint8_t> storage(DfeReceiver::storage_bytes(config));
    auto detector = std::make_unique<PreambleDetector>();
    auto receiver = std::make_unique<DfeReceiver>();
    REQUIRE(receiver->configure(config, storage, dec));

    // WN 1: the largest equaliser (K = 48, 54 taps). WN 6: the QPSK turbo loop. WN 7 near its
    // mask: the 8PSK turbo loop, its salvage and the MFB ensemble. WN 8: the QAM16 MFB decoder.
    const std::array<std::pair<int, double>, 4> cases{{{1, 0.0}, {6, 12.0}, {7, 13.0}, {8, 19.0}}};
    std::mt19937 rng(3);
    std::int64_t turbo_blocks = 0;
    for (const auto& [wn, snr] : cases) {
        INFO("wn " << wn);
        TxSettings s;
        s.waveform_number = wn;
        const auto ip = interleaver_3k(wn, InterleaverKind::Short);
        TxRig rig(s, 2 * static_cast<std::size_t>(ip->input_bits));
        const auto payload = random_bits(rng, static_cast<std::size_t>(ip->input_bits) + 3);
        const auto audio = on_air(rig.burst(payload), snr, 4 + static_cast<std::uint64_t>(wn));
        detector->reset();
        receiver->reset();

        int blocks = 0;
        bool ended = false;
        std::size_t det = 0, rx = 0;
        const std::span<const float> in(audio);
        const ms110d_test::AllocationScope scope;
        while (rx < in.size() && !ended) {
            if (!receiver->active() && receiver->event() == DfeEvent::None) {
                det += detector->process(in.subspan(det, std::min<std::size_t>(512, in.size() - det)));
                while (rx < det) rx += receiver->push_audio(in.subspan(rx, det - rx));
                PreambleDetection d;
                if (detector->take_detection(d)) receiver->start(dfe_start_from_detection(d));
                if (det >= in.size()) break;
                continue;
            }
            rx += receiver->push_audio(in.subspan(rx, std::min<std::size_t>(512, in.size() - rx)));
            while (receiver->event() != DfeEvent::None) {
                if (receiver->event() == DfeEvent::BlockDecoded) {
                    ++blocks;
                } else {
                    ended = true;
                }
                receiver->take_event();
            }
        }
        const std::size_t seen = scope.allocations();
        CHECK(seen == 0);
        CHECK(ended);
        CHECK(blocks == 2);
        const auto& st = receiver->stats();
        turbo_blocks += st.turbo_converged + st.turbo_reverted + st.turbo_aborted;
    }
    CHECK(turbo_blocks == 8);  // every block went through the turbo stage or the MFB decoder
}

TEST_CASE("dfe receiver: storage, configuration and refusals", "[dfe_receiver]") {
    // The memory each configuration needs (docs/dev/dfe-receiver.md has the table).
    for (const InterleaverKind il :
         {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium, InterleaverKind::Long}) {
        DfeReceiverConfig c;
        c.max_interleaver = il;
        std::printf("dfe receiver storage, up to interleaver %d: ring %zu T/2 samples, %zu bytes, decoders for %zu info bits, "
                    "object %zu bytes\n",
                    static_cast<int>(il), DfeReceiver::ring_size_for(il), DfeReceiver::storage_bytes(c),
                    DfeReceiver::max_input_bits_for(il), sizeof(DfeReceiver));
    }
    CHECK(DfeReceiver::ring_size_for(InterleaverKind::Long) == 65536);  // the C#'s ring
    CHECK(DfeReceiver::ring_size_for(InterleaverKind::Short) == 8192);

    DfeReceiverConfig config;
    config.max_interleaver = InterleaverKind::Short;
    const std::size_t bits = DfeReceiver::max_input_bits_for(config.max_interleaver);
    fec::TailBitingViterbiDecoder k7(fec::k7_133_171, bits);
    fec::TailBitingViterbiDecoder k9(fec::k9_561_753, bits);
    fec::TailBitingSisoDecoder s7(fec::k7_133_171, bits);
    fec::TailBitingSisoDecoder s9(fec::k9_561_753, bits);
    BasicChainBcjr<float> chain(DfeReceiver::max_frame_symbols, 16);
    DfeDecoders dec;
    dec.viterbi_k7 = &k7;
    dec.viterbi_k9 = &k9;
    dec.siso_k7 = &s7;
    dec.siso_k9 = &s9;
    dec.chain = &chain;
    std::vector<std::uint8_t> storage(DfeReceiver::storage_bytes(config));
    auto rx = std::make_unique<DfeReceiver>();
    CHECK_FALSE(rx->configure(config, {storage.data(), storage.size() - 1}, dec));
    DfeDecoders missing = dec;
    missing.chain = nullptr;
    CHECK_FALSE(rx->configure(config, storage, missing));
    DfeReceiverConfig bad = config;
    bad.roll_off = 2.0;
    CHECK_FALSE(rx->configure(bad, storage, dec));
    // Any alignment of the block is accepted.
    std::vector<std::uint8_t> odd(storage.size() + 1);
    CHECK(rx->configure(config, {odd.data() + 1, storage.size()}, dec));
    REQUIRE(rx->configure(config, storage, dec));

    DfeStart s;
    s.waveform = 0;
    CHECK_FALSE(rx->start(s));  // WN 0 is the Walsh receiver's
    s.waveform = 9;
    CHECK_FALSE(rx->start(s));  // not implemented
    s.waveform = 1;
    s.interleaver = InterleaverKind::Long;
    CHECK_FALSE(rx->start(s));  // longer than configured
    s.interleaver = InterleaverKind::Short;
    s.constraint_length = 8;
    CHECK_FALSE(rx->start(s));
    s.constraint_length = 9;
    CHECK(rx->start(s));
    CHECK(rx->active());
    CHECK_FALSE(rx->start(s));  // one burst at a time
    rx->reset();
    CHECK_FALSE(rx->active());
    CHECK(rx->event() == DfeEvent::None);
}

TEST_CASE("dfe receiver: soft depuncturing inverts the puncture map", "[dfe_receiver]") {
    for (const auto* code : {&fec::k7_133_171, &fec::k9_561_753}) {
        for (const Mode& m : modes_3k()) {
            const PunctureSpec* spec = find_puncture(*code, static_cast<unsigned>(m.code_rate.numerator),
                                                     static_cast<unsigned>(m.code_rate.denominator));
            REQUIRE(spec != nullptr);
            const std::size_t info = 144;
            const auto len = punctured_length(*spec, info);
            REQUIRE(len);
            // Number every mother position, puncture the numbers, spread them back.
            std::vector<std::uint8_t> index(2 * info);
            std::vector<float> rx(*len);
            std::vector<std::uint8_t> punctured(*len);
            std::vector<float> expect(2 * info, 0.0f);
            for (std::size_t i = 0; i < 2 * info; ++i) index[i] = static_cast<std::uint8_t>(i % 251);
            REQUIRE(apply_puncture(*spec, index, punctured) == *len);
            for (std::size_t j = 0; j < *len; ++j) rx[j] = 1.0f + static_cast<float>(j % 7);
            // Expected: each mother position receives the sum of the values punctured from it.
            {
                std::size_t j = 0, col = 0;
                for (std::size_t n = 0; n < info; ++n) {
                    for (unsigned r = 0; r < spec->repeat; ++r) {
                        if ((spec->keep_t1 >> col) & 1u) expect[2 * n] += rx[j++];
                        if ((spec->keep_t2 >> col) & 1u) expect[2 * n + 1] += rx[j++];
                        if (++col == spec->columns) col = 0;
                    }
                }
            }
            std::vector<float> mother(2 * info, 99.0f);
            CHECK(depuncture_soft(*spec, rx, mother));
            CHECK(mother == expect);
            CHECK_FALSE(depuncture_soft(*spec, {rx.data(), rx.size() - 1}, mother));
        }
    }
}

TEST_CASE("dfe receiver: the two probe pattern rules and the evidence", "[dfe_receiver]") {
    ProbePattern p;
    p.configure(16, 256);  // WN 1 short
    CHECK_FALSE(p.boundary(-1));
    CHECK(p.current(14));
    CHECK_FALSE(p.standard(14));
    CHECK(p.standard(254));
    CHECK(p.current(254));
    CHECK(p.needs_evidence(14));
    CHECK_FALSE(p.needs_evidence(254));
    CHECK(p.boundary(14));  // the current rule until the standard one is proven
    p.add_evidence(0.49);
    CHECK(p.boundary(14));
    p.add_evidence(0.02);
    CHECK_FALSE(p.boundary(14));  // switched at +0.5
    p.add_evidence(-4.0);
    CHECK(p.locked());
    CHECK_FALSE(p.needs_evidence(14));
    CHECK(p.boundary(14));
    p.configure(256, 256);  // Long: the rules agree everywhere
    for (std::int64_t g = -1; g < 600; ++g) CHECK_FALSE(p.needs_evidence(g));
}
