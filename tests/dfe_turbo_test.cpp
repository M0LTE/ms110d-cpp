// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The shipped receive path, turbo stage and MFB decoder included, against the real C#
// demodulator: every WN 1 to 8 and 13 reference case (the rx-turbo dump), and the corpus of Poor,
// Moderate and shifted multipath bursts with carrier offsets and of standard probe pattern bursts
// (rx-corpus). Both are made by scripts/generate-rx-dumps.sh. docs/dev/dfe-receiver.md says what
// is compared and why.

#include "alloc_guard.hpp"
#include "bcjr.hpp"
#include "dfe_receiver.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "preamble_detector.hpp"
#include "probe_pattern.hpp"
#include "reference_files.hpp"
#include "rx_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
namespace fs = std::filesystem;

namespace {

struct Fnv {
    std::uint64_t h = 14695981039346656037ULL;
    void add(std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 1099511628211ULL;
        }
    }
    void add(double d) {
        const auto v = std::bit_cast<std::uint64_t>(d);
        add(static_cast<std::uint32_t>(v));
        add(static_cast<std::uint32_t>(v >> 32));
    }
    void add(float f) { add(std::bit_cast<std::uint32_t>(f)); }
};

std::string hex(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// "<n> <hash>" of a bit sequence, as the C# BitHash.
template <class It>
std::string bit_hash(It begin, It end) {
    Fnv f;
    std::size_t n = 0;
    for (It i = begin; i != end; ++i, ++n) f.add(static_cast<std::uint32_t>(*i));
    return std::to_string(n) + " " + hex(f.h);
}

std::string frame_hash(const std::vector<DfeFrameTrace>& frames, std::size_t from, std::size_t to) {
    Fnv f;
    for (std::size_t i = from; i < to; ++i) {
        const DfeFrameTrace& t = frames[i];
        for (const double d : {t.omega, t.theta_base, t.tau, t.probe_gain_ref, t.probe_energy_ref, t.probe_mse,
                               t.fade_floor, t.presence, t.presence_plain, t.pattern_evidence}) {
            f.add(d);
        }
        for (int k = 0; k < t.tap_count; ++k) {
            f.add(t.taps[static_cast<std::size_t>(k)].real());
            f.add(t.taps[static_cast<std::size_t>(k)].imag());
        }
        f.add(static_cast<std::uint32_t>(t.fading_latched ? 1 : 0));
        f.add(static_cast<std::uint32_t>(t.fading ? 1 : 0));
        f.add(static_cast<std::uint32_t>(t.frames_since_excursion));
    }
    return hex(f.h);
}

const char* outcome_name(DfeTurbo t) {
    switch (t) {
        case DfeTurbo::Skipped: return "skipped";
        case DfeTurbo::Converged: return "converged";
        case DfeTurbo::Salvaged: return "salvaged";
        case DfeTurbo::Reverted: return "reverted";
        case DfeTurbo::Aborted: return "aborted";
    }
    return "?";
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

std::map<std::string, std::string> pairs(std::istringstream& in) {
    std::map<std::string, std::string> m;
    std::string k, v;
    while (in >> k >> v) m[k] = v;
    return m;
}

// One burst as the C# dump has it.
struct CsBlock {
    long long at = 0;
    std::string outcome;
    int mfb_offered = 0, mfb_selected = 0, relock_offers = 0, relock_adopted = 0, frames = 0;
    std::string fhash;
    std::map<std::string, std::string> last;
    std::string first_hash, first_signs;  // first_signs: "<n> <hash>"
    double first_sum = 0;
    bool turbo = false;
    std::string turbo_signs;  // hex nibbles
    double turbo_sum = 0;
    std::string info;  // "<n> <hash>"
};

struct CsBurst {
    std::string label;
    std::map<std::string, std::string> head;  // burst line (corpus)
    std::vector<std::uint8_t> payload;
    int wn = -1, frames = 0;
    std::vector<CsBlock> blocks;
    std::string reason, payload_hash;
    int end_blocks = 0;
    long long payload_bits = 0, end_at = 0;
};

std::vector<CsBurst> load(const char* path, const std::string& head_kind) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::vector<CsBurst> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == head_kind) {
            CsBurst b;
            s >> b.label;
            b.head = pairs(s);
            if (b.head.count("wn") != 0) b.wn = std::stoi(b.head["wn"]);
            if (b.head.count("frames") != 0) b.frames = std::stoi(b.head["frames"]);
            out.push_back(b);
        } else if (kind == "payload") {
            std::string bits;
            s >> bits;
            for (const char c : bits) out.back().payload.push_back(c == '1' ? 1 : 0);
        } else if (kind == "wn") {
            CsBurst& b = out.back();
            std::string k;
            s >> b.wn >> k >> b.frames;
        } else if (kind == "block") {
            CsBlock k;
            int index = 0;
            std::string w;
            s >> index >> k.at >> k.outcome >> w >> k.mfb_offered >> k.mfb_selected >> w >> k.relock_offers >> k.relock_adopted >>
                w >> k.frames >> w >> k.fhash;
            out.back().blocks.push_back(k);
        } else if (kind == "last") {
            out.back().blocks.back().last = pairs(s);
        } else if (kind == "first") {
            CsBlock& k = out.back().blocks.back();
            std::string n, h;
            s >> k.first_hash >> n >> h >> k.first_sum;
            k.first_signs = n + " " + h;
        } else if (kind == "turbo") {
            CsBlock& k = out.back().blocks.back();
            std::string first;
            s >> first;
            if (first != "-") {
                k.turbo = true;  // `first` is the hash of the float bits; the signs and sum follow
                s >> k.turbo_signs >> k.turbo_sum;
            }
        } else if (kind == "info") {
            std::string n, h;
            s >> n >> h;
            out.back().blocks.back().info = n + " " + h;
        } else if (kind == "end") {
            CsBurst& b = out.back();
            std::string n, h;
            s >> b.reason >> b.end_blocks >> b.payload_bits >> b.end_at >> n >> h;
            b.payload_hash = n + " " + h;
        }
    }
    return out;
}

// The default build reproduces the C# exactly through the turbo stage (both run the equaliser
// and the chain BCJR in the same precision), so every decision must match. The single build
// keeps the equaliser's running sums in float where the C# uses double; on a hard fading or
// drifting burst that can move the tracking and so the first pass and the turbo outcomes (by how
// much depends on the machine's libm too), so there the differences are counted and the decoding
// success is held to the C#'s instead.
#ifdef MS110D_SINGLE_PRECISION
constexpr bool strict = false;
#else
constexpr bool strict = true;
#endif

// What a comparison found, over all bursts.
struct Tally {
    long bursts = 0, blocks = 0, frames = 0;
    long fhash_equal = 0, first_hash_equal = 0;
    long turbo_blocks = 0, turbo_bits = 0, turbo_sign_flips = 0;
    double first_sum = 0, turbo_sum = 0, last_state = 0;
    std::map<std::string, long> outcomes;
    long mfb_offered = 0, mfb_selected = 0;
    long relock_offers = 0, relock_adopted = 0, relock_salvages = 0;
    std::map<std::string, long> ends;
    // Single build only: what differed from the C#.
    long bursts_diff = 0, blocks_diff = 0;
    // Bursts whose decoded payload is the one sent, by each.
    long payload_right = 0, cs_payload_right = 0, payload_known = 0;
};

// Compares one C++ run with the C#'s record of the same audio. `sent` is the payload transmitted,
// when known.
void compare(const CsBurst& c, const ms110d_test::RxRun& r, Tally& t, const std::vector<std::uint8_t>* sent) {
    INFO("burst " << c.label);
    ++t.bursts;
    if (sent != nullptr) {
        ++t.payload_known;
        if (c.reason == "Eom" && c.payload_hash == bit_hash(sent->begin(), sent->end())) ++t.cs_payload_right;
        if (r.ended && r.end.reason == DfeEndReason::Eom) {
            std::vector<std::uint8_t> got;
            for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
            got.resize(static_cast<std::size_t>(std::max<long long>(0, r.end.payload_bits)));
            if (got == *sent) ++t.payload_right;
        }
    }
    if (c.wn < 0) {
        // The C# never locked (it does not acquire a one super-frame preamble, nor does this).
        CHECK(r.frames.empty());
        CHECK(r.blocks.empty());
        ++t.ends["no lock"];
        return;
    }
    REQUIRE(r.detected);
    CHECK(r.detection.waveform == c.wn);
    ++t.ends[c.reason];
    const bool same_shape = r.frames.size() == static_cast<std::size_t>(c.frames) && r.blocks.size() == c.blocks.size();
    if (!strict && !same_shape) {
        ++t.bursts_diff;
        return;
    }
    CHECK(r.frames.size() == static_cast<std::size_t>(c.frames));
    REQUIRE(r.blocks.size() == c.blocks.size());
    t.frames += c.frames;
    bool burst_differs = false;
    for (std::size_t i = 0; i < c.blocks.size(); ++i) {
        INFO("block " << i);
        const CsBlock& k = c.blocks[i];
        const ms110d_test::RxBlockRecord& b = r.blocks[i];
        ++t.blocks;
        CHECK(b.at_t2 == k.at);
        ++t.outcomes[k.outcome];
        t.mfb_offered += k.mfb_offered;
        t.mfb_selected += k.mfb_selected;
        t.relock_offers += k.relock_offers;
        t.relock_adopted += k.relock_adopted;
        if (k.outcome == "salvaged" && k.relock_offers > 0) ++t.relock_salvages;  // on the late-lock rung
        REQUIRE(b.frames_end - b.frames_before == static_cast<std::size_t>(k.frames));
        if (frame_hash(r.frames, b.frames_before, b.frames_end) == k.fhash) ++t.fhash_equal;
        if (k.frames > 0) {
            const DfeFrameTrace& f = r.frames[b.frames_end - 1];
            const auto d = [&k](const char* key) { return std::stod(k.last.at(key)); };
            t.last_state = std::max({t.last_state, rel(f.omega, d("omega")), std::abs(f.theta_base - d("theta")),
                                     std::abs(f.tau - d("tau")), rel(f.probe_gain_ref, d("gainref")),
                                     rel(f.presence, d("presence")), std::abs(f.pattern_evidence - d("evidence"))});
            CHECK(f.fading_latched == (k.last.at("latched") == "1"));
        }

        // First pass: signs exact; the float bits hash and the magnitudes reported.
        Fnv fh;
        double sum = 0;
        for (const float x : b.llrs) {
            fh.add(x);
            sum += std::abs(static_cast<double>(x));
        }
        if (hex(fh.h) == k.first_hash) ++t.first_hash_equal;
        std::vector<std::uint8_t> signs;
        for (const float x : b.llrs) signs.push_back(x < 0 ? 1 : 0);
        const bool signs_same = bit_hash(signs.begin(), signs.end()) == k.first_signs;
        if (strict) CHECK(signs_same);
        t.first_sum = std::max(t.first_sum, rel(sum, k.first_sum));

        // The turbo stage: what it did, its last iterate and the decode.
        const bool block_same = signs_same && std::string(outcome_name(b.turbo)) == k.outcome &&
                                static_cast<int>(b.mfb_offered) == k.mfb_offered &&
                                static_cast<int>(b.mfb_selected) == k.mfb_selected &&
                                b.relock_offers == k.relock_offers && b.relock_adopted == k.relock_adopted &&
                                !b.turbo_llrs.empty() == k.turbo && bit_hash(b.info.begin(), b.info.end()) == k.info;
        if (!strict && !block_same) {
            ++t.blocks_diff;
            burst_differs = true;
            continue;
        }
        CHECK(std::string(outcome_name(b.turbo)) == k.outcome);
        CHECK(static_cast<int>(b.mfb_offered) == k.mfb_offered);
        CHECK(static_cast<int>(b.mfb_selected) == k.mfb_selected);
        CHECK(b.relock_offers == k.relock_offers);
        CHECK(b.relock_adopted == k.relock_adopted);
        CHECK(!b.turbo_llrs.empty() == k.turbo);
        if (k.turbo && !b.turbo_llrs.empty()) {
            ++t.turbo_blocks;
            double tsum = 0;
            for (std::size_t j = 0; j < b.turbo_llrs.size(); ++j) {
                const int nib = std::stoi(std::string(1, k.turbo_signs[j / 4]), nullptr, 16);
                const bool neg = ((nib >> (j % 4)) & 1) != 0;
                if (neg != (b.turbo_llrs[j] < 0)) ++t.turbo_sign_flips;
                tsum += std::min(std::abs(static_cast<double>(b.turbo_llrs[j])), 30.0);
                ++t.turbo_bits;
            }
            if (rel(tsum, k.turbo_sum) > 1e-3 && std::getenv("MS110D_TURBO_DEBUG") != nullptr) {
                std::printf("  turbo magnitudes %s block %zu: %.6g against %.6g\n", c.label.c_str(), i, tsum, k.turbo_sum);
            }
            t.turbo_sum = std::max(t.turbo_sum, rel(tsum, k.turbo_sum));
        }
        CHECK(bit_hash(b.info.begin(), b.info.end()) == k.info);
    }
    if (burst_differs) {
        ++t.bursts_diff;
        return;
    }
    if (c.reason == "None") {
        CHECK_FALSE(r.ended);
    } else {
        REQUIRE(r.ended);
        CHECK(std::string(reason_name(r.end.reason)) == c.reason);
        CHECK(r.end.blocks == c.end_blocks);
        CHECK(r.end.payload_bits == c.payload_bits);
        CHECK(r.end.at_t2 == c.end_at);
        std::vector<std::uint8_t> got;
        for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
        got.resize(static_cast<std::size_t>(std::max<long long>(0, r.end.payload_bits)));
        CHECK(bit_hash(got.begin(), got.end()) == c.payload_hash);
    }
}

void require_file(const std::string& path) {
    if (std::ifstream(path).good()) return;
    if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("missing: " << path);
    SKIP("no file at " << path << " (run scripts/generate-rx-dumps.sh)");
}

void print(const char* what, const Tally& t) {
    std::printf("%s: %ld bursts, %ld blocks, %ld frames; frame state hashes equal %ld of %ld, first-pass hashes equal %ld, "
                "first-pass magnitudes %.3g, last frame state %.3g; turbo iterates %ld, sign differences %ld of %ld, "
                "magnitudes %.3g; MFB offered %ld selected %ld; late-lock offers %ld adopted %ld, salvages on that rung %ld; outcomes",
                what, t.bursts, t.blocks, t.frames, t.fhash_equal, t.blocks, t.first_hash_equal, t.first_sum, t.last_state,
                t.turbo_blocks, t.turbo_sign_flips, t.turbo_bits, t.turbo_sum, t.mfb_offered, t.mfb_selected, t.relock_offers,
                t.relock_adopted, t.relock_salvages);
    for (const auto& [k, v] : t.outcomes) std::printf(" %s %ld", k.c_str(), v);
    std::printf("; ends");
    for (const auto& [k, v] : t.ends) std::printf(" %s %ld", k.c_str(), v);
    std::printf("; payload right %ld of %ld (C# %ld); differing from the C#: %ld bursts, %ld blocks\n", t.payload_right,
                t.payload_known, t.cs_payload_right, t.bursts_diff, t.blocks_diff);
}

// The bounds for both comparisons. The default build reproduces the C# frame state to the bit (the
// same libm on the machine the dumps are made on, as the CI reference job does), so every frame
// hash must match there; that is what makes a change to any tracking constant fail. The single
// build differs in the equaliser's sums only, so its frame values are held to a tolerance.
// `max_diff_quarters`: how many quarters of the bursts the single build may take a different
// path on (see check_tally's body).
void check_tally(const Tally& t, int max_diff_quarters) {
    if (strict) {
        CHECK(t.fhash_equal == t.blocks);
        CHECK(t.bursts_diff == 0);
        CHECK(t.payload_right == t.cs_payload_right);
    } else {
        // The single build decodes at least as many bursts right as the C#, give or take one. And it
        // may take a different path from the C# only on a bounded share of bursts: none of the clean
        // reference cases, at most a quarter of the channel corpus (measured: 17 of 166 here, about
        // 10%; the CI runner's libm moves more). The divergence comes from the equaliser's float sums
        // tipping marginal fading blocks, so it stays a minority; a broken single build would leave
        // most bursts.
        CHECK(t.payload_right + 1 >= t.cs_payload_right);
        CHECK(t.bursts_diff * 4 <= t.bursts * max_diff_quarters);
    }
    if (strict) {
        CHECK(t.first_sum <= 1e-4);
        CHECK(t.last_state <= 1e-3);
        // Measured: every turbo soft value identical.
        CHECK(t.turbo_sum <= 1e-6);
        CHECK(t.turbo_sign_flips == 0);
    } else {
        // Measured: magnitudes within 0.13 and 0.3% of signs different, on iterates whose earlier
        // rounds already differed; the decisions that matter are the payloads, checked above.
        CHECK(t.turbo_sum <= 0.25);
        CHECK(t.turbo_sign_flips * 100 <= t.turbo_bits);
    }
}

}  // namespace

TEST_CASE("reference demodulator: the shipped receiver on every WN 1 to 8 and 13 reference case against the C#",
          "[dfe_turbo]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    require_file(MS110D_CSHARP_RX_TURBO);
    const std::vector<CsBurst> cases = load(MS110D_CSHARP_RX_TURBO, "case");
    REQUIRE(cases.size() == 112);
    ms110d_test::RxDriver driver(InterleaverKind::Long);
    Tally t;
    for (const CsBurst& c : cases) {
        const ms110d_test::ReferenceCase rc(dir / c.label, c.label);
        std::vector<float> audio = rc.real32("audio_f32le");
        audio.resize(audio.size() + 9600, 0.0f);
        const auto payload = rc.bytes("payload_bits");
        compare(c, driver.run(audio), t, &payload);
        // Clean: the payload is the one sent, where the burst ends at its EOM.
        if (c.reason == "Eom") CHECK(c.payload_hash == bit_hash(payload.begin(), payload.end()));
    }
    print("shipped receiver vs C#, 112 clean cases", t);
    check_tally(t, 0);
}

TEST_CASE("reference demodulator: Poor, Moderate and shifted multipath with carrier offsets, and the standard probe "
          "pattern, against the C#",
          "[dfe_turbo]") {
    const std::string stem = MS110D_RX_CORPUS_STEM;
    require_file(stem + ".txt");
    require_file(stem + ".i16le");
    const std::vector<CsBurst> bursts = load((stem + ".txt").c_str(), "burst");
    REQUIRE(bursts.size() >= 100);
    std::ifstream bin(stem + ".i16le", std::ios::binary);
    REQUIRE(bin.good());
    ms110d_test::RxDriver driver(InterleaverKind::Medium);
    Tally t;
    long clamp_frames = 0, fade_enters = 0, fade_exits = 0, fade_reentries = 0, floor_recoveries = 0, std_switched = 0,
         std_bursts = 0, cur_kept = 0, cur_bursts = 0, std_long_bursts = 0, std_long_reached = 0;
    const double window_3hz = 2.0 * std::numbers::pi * 3.0 / (2.0 * 2400);
    for (const CsBurst& c : bursts) {
        const long long offset = std::stoll(c.head.at("offset"));
        const long long samples = std::stoll(c.head.at("samples"));
        std::vector<std::int16_t> q(static_cast<std::size_t>(samples));
        bin.seekg(offset);
        bin.read(reinterpret_cast<char*>(q.data()), static_cast<std::streamsize>(2 * samples));
        REQUIRE(bin.good());
        std::vector<float> audio(q.size());
        for (std::size_t i = 0; i < q.size(); ++i) audio[i] = static_cast<float>(q[i] / 8192.0);
        const ms110d_test::RxRun r = driver.run(audio);
        compare(c, r, t, &c.payload);

        // What the bursts exercise (the coverage the C# comparison above is only as good as).
        bool exited = false;
        for (std::size_t i = 0; i < r.frames.size(); ++i) {
            const DfeFrameTrace& f = r.frames[i];
            if (std::abs(std::abs(f.omega - f.omega_acquired) - window_3hz) < 1e-12) ++clamp_frames;
            if (i > 0) {
                const DfeFrameTrace& p = r.frames[i - 1];
                if (f.fading && !p.fading) {
                    ++fade_enters;
                    if (exited) ++fade_reentries;
                }
                if (!f.fading && p.fading) {
                    ++fade_exits;
                    exited = true;
                }
                if (f.fade_floor > p.fade_floor && !f.fresh_solve) ++floor_recoveries;
            }
        }
        // The standard pattern: the receiver switches to it and decodes; the current one it keeps.
        const bool standard = c.head.at("standard") == "1";
        const bool disagree = c.head.at("il") != "3";  // the rules agree everywhere only for Long
        if (c.label.rfind("std-long-", 0) == 0) {
            // Long enough to reach the probe only the standard rule shifts: frame L - 2 of the burst.
            ++std_long_bursts;
            const auto long_il = interleaver_3k(c.wn, InterleaverKind::Long);
            REQUIRE(long_il);
            if (r.frames.size() >= static_cast<std::size_t>(long_il->frames)) ++std_long_reached;
        }
        if (disagree && !r.frames.empty()) {
            const double evidence = r.frames.back().pattern_evidence;
            if (standard) {
                ++std_bursts;
                if (evidence >= ProbePattern::switch_evidence) {
                    ++std_switched;
                } else if (std::getenv("MS110D_TURBO_DEBUG") != nullptr) {
                    std::printf("  standard pattern not taken: %s, evidence %.3f, %zu frames\n", c.label.c_str(), evidence,
                                r.frames.size());
                }
            } else if (c.label.rfind("cur-", 0) == 0) {
                ++cur_bursts;
                if (evidence < ProbePattern::switch_evidence) ++cur_kept;
            }
        }
        if (standard) {
            // Every standard pattern burst decodes, in both builds.
            INFO("burst " << c.label);
            REQUIRE(r.ended);
            CHECK(r.end.reason == DfeEndReason::Eom);
            std::vector<std::uint8_t> got;
            for (const auto& blk : r.blocks) got.insert(got.end(), blk.info.begin(), blk.info.end());
            got.resize(static_cast<std::size_t>(r.end.payload_bits));
            CHECK(got == c.payload);
        }
    }
    print("shipped receiver vs C#, corpus", t);
    check_tally(t, 1);
    std::printf("corpus coverage: frames at the 3 Hz carrier clamp %ld, fade entries %ld, exits %ld, re-entries %ld, fade floor "
                "recoveries %ld; standard pattern bursts switched %ld of %ld (%ld of %ld long ones reach frame L - 2), current "
                "pattern bursts kept %ld of %ld\n",
                clamp_frames, fade_enters, fade_exits, fade_reentries, floor_recoveries, std_switched, std_bursts, std_long_reached,
                std_long_bursts, cur_kept, cur_bursts);
    CHECK(clamp_frames > 0);
    CHECK(fade_enters > 0);
    CHECK(fade_exits > 0);
    CHECK(fade_reentries > 0);
    CHECK(std_long_bursts == 3);
    CHECK(std_long_reached == std_long_bursts);
    CHECK(t.relock_salvages > 0);
    CHECK(t.relock_adopted > 0);
    CHECK(t.relock_adopted < t.relock_offers);  // offers refused as well as taken
    CHECK(floor_recoveries > 0);
    CHECK(std_bursts == 39);
    // All but two: WN 1 and WN 2 with the ultra-short interleaver and two blocks offer only two
    // disagreeing probes, whose scores sum to 0.40 against the 0.5 needed to switch (the same in the
    // C#, frame for frame); both still decode their payload.
    CHECK(std_switched == std_bursts - 2);
    CHECK(cur_kept == cur_bursts);
    CHECK(t.outcomes["reverted"] > 0);
    CHECK(t.outcomes["salvaged"] > 0);
    CHECK(t.mfb_selected > 0);
}

TEST_CASE("reference demodulator: nothing allocates over the whole corpus, every turbo path included", "[dfe_turbo]") {
    const std::string stem = MS110D_RX_CORPUS_STEM;
    require_file(stem + ".txt");
    require_file(stem + ".i16le");
    const std::vector<CsBurst> bursts = load((stem + ".txt").c_str(), "burst");
    std::ifstream bin(stem + ".i16le", std::ios::binary);
    REQUIRE(bin.good());

    DfeReceiverConfig config;
    config.max_interleaver = InterleaverKind::Medium;
    const std::size_t bits = DfeReceiver::max_input_bits_for(config.max_interleaver);
    m0lte::fec::TailBitingViterbiDecoder k7(m0lte::fec::k7_133_171, bits);
    m0lte::fec::TailBitingViterbiDecoder k9(m0lte::fec::k9_561_753, bits);
    m0lte::fec::TailBitingSisoDecoder s7(m0lte::fec::k7_133_171, bits);
    m0lte::fec::TailBitingSisoDecoder s9(m0lte::fec::k9_561_753, bits);
    BasicChainBcjr<float> chain(DfeReceiver::max_frame_symbols, 16);
    DfeDecoders dec{&k7, &k9, &s7, &s9, &chain};
    std::vector<std::uint8_t> storage(DfeReceiver::storage_bytes(config));
    auto detector = std::make_unique<PreambleDetector>();
    auto receiver = std::make_unique<DfeReceiver>();
    REQUIRE(receiver->configure(config, storage, dec));

    std::size_t allocations = 0;
    std::int64_t salvaged = 0, reverted = 0, mfb_selected = 0, relock_offers = 0, blocks = 0;
    for (const CsBurst& c : bursts) {
        const long long offset = std::stoll(c.head.at("offset"));
        const long long samples = std::stoll(c.head.at("samples"));
        std::vector<std::int16_t> q(static_cast<std::size_t>(samples));
        bin.seekg(offset);
        bin.read(reinterpret_cast<char*>(q.data()), static_cast<std::streamsize>(2 * samples));
        std::vector<float> audio(q.size());
        for (std::size_t i = 0; i < q.size(); ++i) audio[i] = static_cast<float>(q[i] / 8192.0);
        detector->reset();
        receiver->reset();
        const std::span<const float> in(audio);
        bool ended = false;
        std::size_t det = 0, rx = 0;
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
                    relock_offers += receiver->block().relock_offers;
                    ++blocks;
                } else {
                    ended = true;
                }
                receiver->take_event();
            }
        }
        allocations += scope.allocations();
        const DfeReceiverStats& st = receiver->stats();
        salvaged += st.turbo_salvaged;
        reverted += st.turbo_reverted;
        mfb_selected += st.mfb_selected;
    }
    std::printf("corpus with no allocation: %zu allocations over %zu bursts, %lld blocks; reverted %lld, salvaged %lld, "
                "late-lock offers %lld, MFB selections %lld\n",
                allocations, bursts.size(), static_cast<long long>(blocks), static_cast<long long>(reverted),
                static_cast<long long>(salvaged), static_cast<long long>(relock_offers), static_cast<long long>(mfb_selected));
    CHECK(allocations == 0);
    CHECK(reverted > 0);
    CHECK(salvaged > 0);
    CHECK(relock_offers > 0);
    CHECK(mfb_selected > 0);
}
