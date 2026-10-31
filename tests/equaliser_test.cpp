// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The equaliser (src/equaliser.*) against the C# Dfe. tests/data/csharp-dfe-0.4.1.txt was made by
// tools/csharp-vectors (dfe mode) from the 0.4.1 package: scripted receive scenarios (seeded
// synthetic bursts through a multipath channel with noise and a slow rotation, and real burst
// symbol streams from the reference files through the same channel) driven through the C# Dfe,
// with its outputs written as the hex of their bits. The inputs are not in the dump: the test
// regenerates them (tests/support/eq_scenario.*) and checks their hashes first. See
// docs/dev/equaliser.md for the tolerances and why they are what they are.

#include "alloc_guard.hpp"
#include "eq_scenario.hpp"
#include "equaliser.hpp"
#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <span>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef MS110D_CSHARP_DFE_DUMP
#error "MS110D_CSHARP_DFE_DUMP must be defined by the build"
#endif

using namespace m0lte::ms110d;
using namespace ms110d_test;

namespace {

using Tokens = std::vector<std::string>;

struct Line {
    std::string head;
    Tokens tok;                              // every token after the head
    std::map<std::string, std::string> kv;   // the key=value ones
};

struct Block {
    Line head;
    std::vector<Line> lines;
};

Line parse_line(const std::string& text) {
    Line l;
    std::istringstream in(text);
    in >> l.head;
    std::string t;
    while (in >> t) {
        l.tok.push_back(t);
        const auto eq = t.find('=');
        if (eq != std::string::npos) l.kv[t.substr(0, eq)] = t.substr(eq + 1);
    }
    return l;
}

const std::vector<Block>& dump() {
    static const std::vector<Block> blocks = [] {
        std::ifstream in(MS110D_CSHARP_DFE_DUMP);
        std::vector<Block> out;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            Line l = parse_line(line);
            if (l.head == "scenario" || l.head == "tir") {
                out.push_back(Block{std::move(l), {}});
            } else if (!out.empty()) {
                out.back().lines.push_back(std::move(l));
            }
        }
        return out;
    }();
    return blocks;
}

float f32(const std::string& hex) {
    const std::uint32_t w = static_cast<std::uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
double f64(const std::string& hex) {
    const std::uint64_t w = std::strtoull(hex.c_str(), nullptr, 16);
    double d;
    std::memcpy(&d, &w, 8);
    return d;
}
std::uint64_t u64(const std::string& hex) { return std::strtoull(hex.c_str(), nullptr, 16); }

Sample cx_at(const Tokens& t, std::size_t i) { return Sample{f32(t[i]), f32(t[i + 1])}; }

// A complex value written "key=RE IM": the token after the one that starts with key=.
Sample cx_kv(const Line& l, const std::string& key) {
    for (std::size_t i = 0; i + 1 < l.tok.size(); i++) {
        if (l.tok[i].rfind(key + "=", 0) == 0) return Sample{f32(l.tok[i].substr(key.size() + 1)), f32(l.tok[i + 1])};
    }
    return Sample{};
}

EqScenario scenario_of(const Line& l) {
    const auto& kv = l.kv;
    EqScenario s;
    s.name = kv.at("name");
    s.source = kv.at("source");
    s.k = std::atoi(kv.at("k").c_str());
    s.u = std::atoi(kv.at("u").c_str());
    s.frames = std::atoi(kv.at("frames").c_str());
    s.ff = std::atoi(kv.at("ff").c_str());
    s.fb = std::atoi(kv.at("fb").c_str());
    s.lead = std::atoi(kv.at("lead").c_str());
    s.init_ridge = f32(kv.at("initridge"));
    s.track_ridge = f32(kv.at("trackridge"));
    s.lambda = f32(kv.at("lambda"));
    s.mu = f32(kv.at("mu"));
    s.noise_var = f64(kv.at("noisevar"));
    s.seed = std::strtoull(kv.at("seed").c_str(), nullptr, 10);
    s.chan = std::atoi(kv.at("chan").c_str());
    s.pass = kv.at("pass");
    s.extras = kv.at("extras") == "1";
    s.quiet = kv.at("quiet") == "1";
    s.train_len = std::atoi(kv.at("trainlen").c_str());
    return s;
}

// What one scenario's comparison found.
struct Agreement {
    double max_y = 0;          // largest |y_cpp - y_cs| over every equalised symbol
    double max_tap = 0;        // largest |tap_cpp - tap_cs| / max |tap_cs| at the end
    double max_probe_mse = 0;  // largest relative difference of the per-probe MSE
    double max_stats = 0;      // largest relative difference of the probe statistics (correlation, error energy)
    bool rows_match = true;
    long symbols = 0;
    long decision_mismatches = 0;
    long y_exact = 0;          // symbols whose bits are identical
    bool exact_everywhere = true;
    bool inputs_match = false;
};

Agreement compare(const Block& b, const EqResult& r) {
    Agreement a;
    const Line& h = b.head;
    a.inputs_match = r.txhash == u64(h.kv.at("txhash")) && r.xhash == u64(h.kv.at("xhash"));
    for (const Line& l : b.lines) {
        if (l.head == "init") {
            const double m = f64(l.kv.at("mse"));
            a.max_probe_mse = std::max(a.max_probe_mse, std::fabs(r.init_mse - m) / std::max(m, 1e-30));
        } else if (l.head == "nested") {
            if (r.nested_seen) a.exact_everywhere = a.exact_everywhere && f32(l.kv.at("ffe")) == r.nested_ffe;
        } else if (l.head == "probe") {
            const auto f = static_cast<std::size_t>(std::atoi(l.kv.at("f").c_str()));
            if (f < r.probes.size()) {
                const double m = f64(l.kv.at("mse"));
                a.max_probe_mse = std::max(a.max_probe_mse, std::fabs(r.probes[f].mse - m) / std::max(m, 1e-30));
                // ProbeStats from train_on_known against the C# loop: correlation, error energy, rows.
                const double e = f64(l.kv.at("err"));
                a.max_stats = std::max(a.max_stats, std::fabs(r.probes[f].error_energy - e) / std::max(e, 1e-30));
                const Sample corr = cx_kv(l, "corr");
                const double cd = std::hypot(static_cast<double>(r.probes[f].corr.real() - corr.real()),
                                             static_cast<double>(r.probes[f].corr.imag() - corr.imag()));
                a.max_stats = std::max(a.max_stats, cd / std::max(1.0, static_cast<double>(std::hypot(corr.real(), corr.imag()))));
                a.rows_match = a.rows_match && r.probes[f].rows == std::atoi(l.kv.at("rows").c_str());
            }
        } else if (l.head == "y") {
            const auto f = static_cast<std::size_t>(std::atoi(l.kv.at("f").c_str()));
            if (f >= r.y.size()) continue;
            // tokens: "f=N" then pairs
            for (std::size_t i = 0; i < r.y[f].size(); i++) {
                const Sample ref = cx_at(l.tok, 1 + (2 * i));
                const Sample got = r.y[f][i];
                const double d = std::hypot(static_cast<double>(got.real() - ref.real()), static_cast<double>(got.imag() - ref.imag()));
                a.max_y = std::max(a.max_y, d);
                a.symbols++;
                if (got.real() == ref.real() && got.imag() == ref.imag()) a.y_exact++;
            }
        } else if (l.head == "d") {
            const auto f = static_cast<std::size_t>(std::atoi(l.kv.at("f").c_str()));
            if (f >= r.d.size()) continue;
            const std::string& ref = l.tok[1];
            for (std::size_t i = 0; i < ref.size(); i++) {
                if (ref[i] != r.d[f][i]) a.decision_mismatches++;
            }
        } else if (l.head == "taps") {
            double peak = 0;
            for (std::size_t i = 0; i < r.taps.size(); i++) {
                const Sample ref = cx_at(l.tok, 2 * i);
                peak = std::max(peak, std::hypot(static_cast<double>(ref.real()), static_cast<double>(ref.imag())));
            }
            for (std::size_t i = 0; i < r.taps.size(); i++) {
                const Sample ref = cx_at(l.tok, 2 * i);
                const double d = std::hypot(static_cast<double>(r.taps[i].real() - ref.real()),
                                            static_cast<double>(r.taps[i].imag() - ref.imag()));
                a.max_tap = std::max(a.max_tap, d / peak);
            }
        }
    }
    a.exact_everywhere = a.exact_everywhere && a.y_exact == a.symbols;
    return a;
}

#ifdef MS110D_SINGLE_PRECISION
constexpr const char* precision_name = "single";
#else
constexpr const char* precision_name = "double";
#endif

// Tolerances. The equaliser's arithmetic is the C#'s (single precision complex, same order of
// operations, no fused multiply-add), so the double build is expected to be bit for bit. The
// single build differs only where the C# uses double (the trace that sets the ridge, and the
// residual sums), which perturbs the ridge in its last bit; the effect on an output is bounded by
// a small multiple of float rounding times the conditioning of the solve. These bounds were set
// above the measured worst case and are tight enough to catch a real error (see the doc).
#ifdef MS110D_SINGLE_PRECISION
constexpr double tol_stats = 1e-5;
constexpr double tol_y_clean = 1e-5;
constexpr double tol_y_noisy = 1e-5;
#else
constexpr double tol_stats = 0.0;
constexpr double tol_y_clean = 0.0;
constexpr double tol_y_noisy = 0.0;
#endif

void run_block(const Block& b, const std::filesystem::path& ref_dir) {
    const EqScenario s = scenario_of(b.head);
    const EqResult r = run_eq_scenario(s, ref_dir);
    REQUIRE(r.txhash != 0);
    const Agreement a = compare(b, r);
    CHECK(a.inputs_match);
    const bool noisy = s.name.find("noisy") != std::string::npos;
    std::printf("AGREEMENT %s %s: symbols=%ld exact=%ld max|dy|=%.3g decision_mismatches=%ld max_tap_rel=%.3g max_probe_mse_rel=%.3g max_stats_rel=%.3g errors cpp=%ld cs=%s\n",
                precision_name, s.name.c_str(), a.symbols, a.y_exact, a.max_y, a.decision_mismatches, a.max_tap,
                a.max_probe_mse, a.max_stats, r.errors, b.lines.back().kv.at("errors").c_str());
    CHECK(a.rows_match);
    CHECK(a.max_stats <= tol_stats);
    // The tap counts, lead and ridges the scenario ran with are the ones the demodulator uses for
    // this probe length (the dump holds the C# parameters; a typo in the table would show here).
    const EqParams params = eq_params_for_probe_length(s.k);
    CHECK(params.ff_taps == s.ff);
    CHECK(params.fb_taps == s.fb);
    CHECK(params.lead == s.lead);
    CHECK(params.init_ridge == s.init_ridge);
    CHECK(params.track_ridge == s.track_ridge);
    CHECK(a.max_y <= (noisy ? tol_y_noisy : tol_y_clean));
    // Decisions must match exactly on clean and moderate inputs; on the noisy ones a symbol sitting
    // within rounding of the slicer could flip, so up to 1 in 200 is allowed (none was seen).
    CHECK(a.decision_mismatches <= (noisy ? a.symbols / 200 : 0));
}

}  // namespace

TEST_CASE("equaliser: synthetic scenarios agree with the C# Dfe", "[equaliser]") {
    int ran = 0;
    for (const Block& b : dump()) {
        if (b.head.head != "scenario" || b.head.kv.at("source") != "synthetic" || b.head.kv.at("quiet") == "1") continue;
        DYNAMIC_SECTION(b.head.kv.at("name")) { run_block(b, {}); }
        ran++;
    }
    CHECK(ran >= 9);
}

TEST_CASE("reference equaliser: real symbol streams agree with the C# Dfe", "[equaliser]") {
    const auto dir = ms110d_test::reference_dir_or_skip();
    int ran = 0;
    for (const Block& b : dump()) {
        if (b.head.head != "scenario" || b.head.kv.at("source") == "synthetic") continue;
        DYNAMIC_SECTION(b.head.kv.at("name")) { run_block(b, dir); }
        ran++;
    }
    CHECK(ran >= 3);
}

namespace {

// Compares the target-impulse-response solves. Lags are integers and must match; the floating
// values within a relative tolerance (exact in the double build).
#ifdef MS110D_SINGLE_PRECISION
constexpr double tol_tir = 1e-4;
#else
constexpr double tol_tir = 0.0;
#endif

double rel(float a, float b) {
    const double d = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    return d / std::max(std::fabs(static_cast<double>(b)), 1e-6);
}

}  // namespace

TEST_CASE("equaliser: target impulse response solves agree with the C#", "[equaliser][tir]") {
    int ran = 0;
    for (const Block& b : dump()) {
        if (b.head.head != "tir") continue;
        DYNAMIC_SECTION(b.head.kv.at("name")) {
            TirScenario t;
            t.name = b.head.kv.at("name");
            t.chan = std::atoi(b.head.kv.at("chan").c_str());
            t.noise_var = f64(b.head.kv.at("noisevar"));
            t.allow_pair = b.head.kv.at("pair") == "1";
            t.second_only_lag = std::atoi(b.head.kv.at("onlylag").c_str());
            const TirResult r = run_tir_scenario(t);
            CHECK(r.xhash == u64(b.head.kv.at("xhash")));
            std::size_t pass = 0;
            double worst = 0;
            for (const Line& l : b.lines) {
                if (l.head == "tirsolve") {
                    REQUIRE(pass < r.passes.size());
                    const auto& got = r.passes[pass++];
                    CHECK(got.t.solved == (l.kv.at("solved") == "1"));
                    CHECK(got.t.lag == std::atoi(l.kv.at("lag").c_str()));
                    CHECK(got.t.lag2 == std::atoi(l.kv.at("lag2").c_str()));
                    worst = std::max({worst, rel(got.t.sse_null, f32(l.kv.at("ssenull"))), rel(got.t.sse_tir, f32(l.kv.at("ssetir"))),
                                      rel(got.ffe, f32(l.kv.at("ffe")))});
                    // c1 and c2 are "re im" pairs: the tokens after "c1=" and "c2=".
                    for (std::size_t i = 0; i + 1 < l.tok.size(); i++) {
                        const bool is_c1 = l.tok[i].rfind("c1=", 0) == 0, is_c2 = l.tok[i].rfind("c2=", 0) == 0;
                        if (!is_c1 && !is_c2) continue;
                        const Sample ref{f32(l.tok[i].substr(3)), f32(l.tok[i + 1])};
                        const Sample have = is_c1 ? got.t.coefficient : got.t.coefficient2;
                        worst = std::max({worst, rel(have.real(), ref.real()), rel(have.imag(), ref.imag())});
                    }
                } else if (l.head == "tiry") {
                    REQUIRE(l.tok.size() == 2 * r.y.size());
                    for (std::size_t i = 0; i < r.y.size(); i++) {
                        const Sample ref = cx_at(l.tok, 2 * i);
                        worst = std::max(worst, std::hypot(static_cast<double>(r.y[i].real() - ref.real()),
                                                           static_cast<double>(r.y[i].imag() - ref.imag())) /
                                                    std::max(1.0, std::hypot(static_cast<double>(ref.real()), static_cast<double>(ref.imag()))));
                    }
                }
            }
            std::printf("TIR %s %s: passes=%zu lag=%d worst_rel=%.3g\n", precision_name, t.name.c_str(), pass,
                        r.passes.empty() ? -1 : r.passes[0].t.lag, worst);
            CHECK(worst <= tol_tir);
        }
        ran++;
    }
    CHECK(ran == 6);
}

namespace {
#ifdef MS110D_SINGLE_PRECISION
long tol_same_seed(long cs) { return std::max(2L, cs / 100); }
#else
long tol_same_seed(long) { return 0; }
#endif
}  // namespace

TEST_CASE("equaliser: symbol error rate against noise matches the C# curve", "[equaliser][ser]") {
    // The C# ran each noise level over 200 frames of 96 symbols (19200 symbols counted) with seed
    // 1000. The same seed in C++ makes the same samples, so the counts should be equal (double
    // build) or within a few symbols (single). Other seeds give independent noise and must land
    // within the spread that noise alone explains.
    int ran = 0;
    long last_same = 0;
    for (const Block& b : dump()) {
        if (b.head.head != "scenario" || b.head.kv.at("quiet") != "1") continue;
        EqScenario s = scenario_of(b.head);
        const long cs_errors = std::atol(b.lines.back().kv.at("errors").c_str());
        const long cs_counted = std::atol(b.lines.back().kv.at("counted").c_str());
        const EqResult same = run_eq_scenario(s, {});
        // One run on other noise (a different seed), where there are enough errors to compare.
        // (Each run is 19200 symbols; the Debug and sanitizer builds are slow, so no more.)
        long other = -1;
        if (cs_errors >= 20) {
            EqScenario o = s;
            o.seed = s.seed + 100;
            other = run_eq_scenario(o, {}).errors;
        }
        std::printf("SER %s noisevar=%.3g counted=%ld cs=%ld cpp_same_seed=%ld cpp_other_seed=%ld\n", precision_name,
                    s.noise_var, cs_counted, cs_errors, same.errors, other);
        // Same samples: the same error count (double build bit for bit; single within a rounding
        // of a borderline symbol).
        CHECK(std::labs(same.errors - cs_errors) <= tol_same_seed(cs_errors));
        // Independent noise: within five Poisson standard deviations of the C# count (errors come
        // in bursts, so this is deliberately wider than one deviation).
        if (other >= 0) {
            CHECK(std::fabs(static_cast<double>(other - cs_errors)) <= 5.0 * std::sqrt(std::max(static_cast<double>(cs_errors), 10.0)));
        }
        // The curve rises with noise.
        CHECK(same.errors >= last_same);
        last_same = same.errors;
        ran++;
    }
    CHECK(ran == 7);
}

TEST_CASE("equaliser: storage per tap set", "[equaliser][memory]") {
    // The memory block per mode, as docs/dev/equaliser.md tabulates it. The tap counts follow
    // the probe length: K = 48 (WN 1, 2) is 32 + 22, K = 32 (WN 3 to 8 and 13) is 24 + 12, and
    // K = 24 (not used at 3 kHz) is 16 + 6.
    for (const int k : {48, 32, 24}) {
        const EqParams p = eq_params_for_probe_length(k);
        const std::size_t bytes = Dfe::storage_bytes(p.ff_taps, p.fb_taps);
        std::printf("RAM K=%d taps=%d+%d bytes=%zu\n", k, p.ff_taps, p.fb_taps, bytes);
        const std::size_t n = static_cast<std::size_t>(p.ff_taps + p.fb_taps);
        // Five n x n complex matrices (Gram, Cholesky factor, subset/seed scratch, saved Gram and
        // RLS P) dominate; the rest is vectors.
        CHECK(bytes > 5 * n * n * 8);
        CHECK(bytes < 5 * n * n * 8 + (24 * n + 8 * static_cast<std::size_t>(p.ff_taps)) * 8 + 64);
    }
    CHECK(Dfe::storage_bytes(32, 22) == 121555);
    CHECK(Dfe::storage_bytes(24, 12) == 55203);
    CHECK(Dfe::storage_bytes(16, 6) == 21459);
    CHECK(Dfe::storage_bytes(0, 4) == 0);
    CHECK(Dfe::storage_bytes(4, -1) == 0);
    CHECK(Dfe::storage_bytes(60, 5) == 0);
    CHECK(Dfe::storage_bytes(64, 0) != 0);
}

TEST_CASE("equaliser: configure checks its storage", "[equaliser][memory]") {
    Dfe d;
    CHECK_FALSE(d.configured());
    std::vector<std::uint8_t> small(100);
    CHECK_FALSE(d.configure(8, 4, small));
    CHECK_FALSE(d.configured());
    CHECK_FALSE(d.configure(0, 4, small));
    std::vector<std::uint8_t> exact(Dfe::storage_bytes(8, 4));
    CHECK(d.configure(8, 4, exact));
    CHECK(d.ff_taps() == 8);
    CHECK(d.fb_taps() == 4);
    // Any alignment: a block that starts one byte in still fits, because storage_bytes() includes the slack.
    std::vector<std::uint8_t> odd(Dfe::storage_bytes(8, 4) + 1);
    Dfe e;
    CHECK(e.configure(8, 4, std::span<std::uint8_t>(odd).subspan(1)));
    // Reconfiguring starts clean: zero taps.
    std::vector<Sample> taps(12);
    e.snapshot_taps(taps);
    for (const Sample& t : taps) CHECK((t.real() == 0 && t.imag() == 0));
}

TEST_CASE("equaliser: calls that the C# would throw on report failure", "[equaliser]") {
    auto storage = eq_storage(4, 2);
    Dfe d;
    REQUIRE(d.configure(4, 2, storage));
    std::vector<Sample> w(4, Sample{1, 0}), p(2, Sample{1, 0});
    CHECK_FALSE(d.add_training_row(w, p, Sample{1, 0}));  // before begin_training
    CHECK_FALSE(d.restore_training());                    // nothing saved
    d.begin_training();
    CHECK(d.add_training_row(w, p, Sample{1, 0}));
    CHECK_FALSE(d.solve_training());  // one row is fewer than the six taps, and not anchored
    CHECK_FALSE(d.add_training_row(w, p, Sample{1, 0}));  // the failed solve consumed the accumulation
    CHECK(d.solve_training(1e-3f, true) == false);        // nothing accumulated now
    // A known-symbol sweep needs the feedback history before its first row.
    d.begin_training();
    std::vector<Sample> known(20, Sample{1, 0});
    auto fill = +[](void*, int, std::span<Sample> window) noexcept {
        for (auto& v : window) v = Sample{0.5f, 0.0f};
    };
    CHECK(d.train_on_known(known, 1, fill, nullptr, 1.0f).rows == 0);
    CHECK(d.train_on_known(known, 2, fill, nullptr, 1.0f).rows == 18);
}

TEST_CASE("equaliser: train_on_known equals the row loop it replaces", "[equaliser]") {
    EqScenario s;
    s.name = "unit";
    s.noise_var = 0.01;
    // The scenario's own initial solve uses add_training_row; do the same solve here with
    // train_on_known over a made-up window and compare the taps.
    auto st1 = eq_storage(6, 3), st2 = eq_storage(6, 3);
    Dfe a, b;
    REQUIRE(a.configure(6, 3, st1));
    REQUIRE(b.configure(6, 3, st2));
    std::vector<Sample> known;
    for (int i = 0; i < 40; i++) known.push_back(Sample{static_cast<float>(i * 7 % 5) - 2.0f, static_cast<float>(i * 3 % 4) - 1.5f});
    struct Ctx { const std::vector<Sample>* k; } ctx{&known};
    auto fill = +[](void* c, int i, std::span<Sample> window) noexcept {
        const auto& k = *static_cast<Ctx*>(c)->k;
        for (std::size_t j = 0; j < window.size(); j++) {
            const std::size_t at = (static_cast<std::size_t>(i) + j) % k.size();
            window[j] = Sample{k[at].real() * 0.7f + 0.1f * static_cast<float>(j), k[at].imag() * 0.7f - 0.05f * static_cast<float>(i % 3)};
        }
    };
    a.begin_training();
    const ProbeStats stats = a.train_on_known(known, 3, fill, &ctx, 2.0f);
    CHECK(stats.rows == 37);
    b.begin_training();
    std::vector<Sample> w(6), p(3);
    for (int i = 3; i < 40; i++) {
        fill(&ctx, i, w);
        for (int j = 0; j < 3; j++) p[static_cast<std::size_t>(j)] = known[static_cast<std::size_t>(i - 1 - j)];
        b.add_training_row(w, p, known[static_cast<std::size_t>(i)], 2.0f);
    }
    CHECK(a.solve_training(1e-3f, false, 0.0f));
    CHECK(b.solve_training(1e-3f, false, 0.0f));
    std::vector<Sample> ta(9), tb(9);
    a.snapshot_taps(ta);
    b.snapshot_taps(tb);
    for (std::size_t i = 0; i < 9; i++) CHECK((ta[i].real() == tb[i].real() && ta[i].imag() == tb[i].imag()));
}

TEST_CASE("equaliser: nothing allocates after configure", "[equaliser][alloc]") {
    for (const int k : {48, 32, 24}) {
        const EqParams p = eq_params_for_probe_length(k);
        auto storage = eq_storage(p.ff_taps, p.fb_taps);
        Dfe d;
        REQUIRE(d.configure(p.ff_taps, p.fb_taps, storage));
        const std::size_t n = static_cast<std::size_t>(p.ff_taps + p.fb_taps);
        std::vector<Sample> w(static_cast<std::size_t>(p.ff_taps)), past(static_cast<std::size_t>(p.fb_taps)), taps(n), taps2(n);
        std::vector<float> var(static_cast<std::size_t>(p.fb_taps), 0.1f);
        std::vector<Sample> known(64);
        for (std::size_t i = 0; i < known.size(); i++) known[i] = Sample{static_cast<float>(i % 3) - 1.0f, static_cast<float>(i % 5) - 2.0f};
        auto fill = +[](void*, int i, std::span<Sample> window) noexcept {
            for (std::size_t j = 0; j < window.size(); j++) window[j] = Sample{std::sin(0.3f * static_cast<float>(i) + static_cast<float>(j)), 0.2f};
        };
        for (std::size_t i = 0; i < w.size(); i++) w[i] = Sample{std::sin(static_cast<float>(i)), 0.1f};
        for (std::size_t i = 0; i < past.size(); i++) past[i] = Sample{1.0f, 0.0f};
        const ms110d_test::AllocationScope scope;
        d.begin_training();
        d.train_on_known(known, p.fb_taps, fill, nullptr, 1.0f);
        d.add_training_row(w, past, Sample{1, 0}, 0.5f);
        d.add_training_row(w, past, var, Sample{1, 0}, 0.5f);
        d.snapshot_training();
        d.begin_rls(0.97f, 1.0f);
        d.seed_rls_from_training(1e-3f, 1.0f, 0.0f);
        d.restore_training();
        d.solve_training(1e-3f, true, 0.0f);
        d.rls_update(w, past, Sample{1, 0}, 1.0f);
        d.nlms(w, past, Sample{1, 0}, 0.05f);
        d.symmetrize_p(10.0f);
        d.snapshot_taps(taps);
        d.load_taps(taps);
        d.load_interpolated_taps(taps, taps, 0.5f);
        d.translate_taps(taps, taps);
        d.rotate_taps(Sample{0, 1});
        d.equalise(w, past);
        d.begin_training();
        d.train_on_known(known, p.fb_taps, fill, nullptr, 1.0f);
        d.train_on_known(known, p.fb_taps, fill, nullptr, 1.0f);
        const TirSolve t = d.solve_training_tir(1e-3f, 0.0f, p.fb_taps, true, 0);
        const std::size_t seen = scope.allocations();
        CHECK(seen == 0);
        CHECK(t.solved);
    }
}

TEST_CASE("equaliser: genie noise power, interpolated taps, the cap on P and training energy after a restore agree with the C#", "[equaliser][misc]") {
    std::uint64_t xhash = 0;
    const std::vector<MiscLine> mine = run_misc_scenario(xhash);
    const Block* block = nullptr;
    for (const Block& b : dump()) {
        if (b.head.head == "misc-header") block = &b;
    }
    // misc-header is not a block head (only scenario and tir are), so its lines hang off the last
    // block; find them by scanning every line of the file instead.
    std::ifstream in(MS110D_CSHARP_DFE_DUMP);
    std::string text;
    std::vector<Line> theirs;
    std::uint64_t their_hash = 0;
    while (std::getline(in, text)) {
        Line l = parse_line(text);
        if (l.head == "misc-header") their_hash = u64(l.kv.at("xhash"));
        if (l.head == "misc") theirs.push_back(std::move(l));
    }
    (void)block;
    CHECK(xhash == their_hash);
    REQUIRE(theirs.size() == mine.size());
#ifdef MS110D_SINGLE_PRECISION
    constexpr double tol = 1e-5;
#else
    constexpr double tol = 0.0;
#endif
    for (std::size_t i = 0; i < mine.size(); i++) {
        const Line& l = theirs[i];
        REQUIRE(l.tok[0] == mine[i].label);
        REQUIRE(l.tok.size() == mine[i].values.size() + 1);
        double worst = 0;
        for (std::size_t j = 0; j < mine[i].values.size(); j++) {
            const float ref = f32(l.tok[j + 1]);
            const double scale = std::max(1.0, std::fabs(static_cast<double>(ref)));
            worst = std::max(worst, std::fabs(static_cast<double>(mine[i].values[j]) - static_cast<double>(ref)) / scale);
        }
        std::printf("MISC %s %s: worst_rel=%.3g\n", precision_name, mine[i].label.c_str(), worst);
        CHECK(worst <= tol);
    }
    // The cap on P changes the taps (it bites), so the comparison above is not vacuous.
    const auto find = [&](const char* label) -> const MiscLine& {
        for (const MiscLine& m : mine) {
            if (m.label == label) return m;
        }
        FAIL("missing " << label);
        return mine[0];
    };
    CHECK(find("p-uncapped").values != find("p-capped").values);
    // Interpolation lands between the two snapshots, not on either.
    CHECK(find("interp").values != find("noisesolve").values);
    CHECK(find("interp").values != find("rls").values);
}

namespace {

struct ScoreCtx {
    const std::vector<Sample>* k;
};
void score_fill(void* c, int i, std::span<Sample> window) noexcept {
    const auto& k = *static_cast<ScoreCtx*>(c)->k;
    for (std::size_t j = 0; j < window.size(); j++) {
        const std::size_t at = (static_cast<std::size_t>(i) + j) % k.size();
        window[j] = Sample{k[at].real() * 0.6f + 0.05f * static_cast<float>(j), k[at].imag() * 0.6f};
    }
}

}  // namespace

TEST_CASE("equaliser: score_known measures without training", "[equaliser]") {
    auto st1 = eq_storage(6, 3), st2 = eq_storage(6, 3);
    Dfe a, b;
    REQUIRE(a.configure(6, 3, st1));
    REQUIRE(b.configure(6, 3, st2));
    std::vector<Sample> known;
    for (int i = 0; i < 30; i++) known.push_back(Sample{static_cast<float>(i * 7 % 5) - 2.0f, static_cast<float>(i * 3 % 4) - 1.5f});
    ScoreCtx ctx{&known};
    // Give both some taps.
    for (Dfe* d : {&a, &b}) {
        d->begin_training();
        d->train_on_known(known, 3, score_fill, &ctx, 1.0f);
        REQUIRE(d->solve_training(1e-3f, false, 0.0f));
    }
    std::vector<Sample> before(9), after(9);
    a.snapshot_taps(before);
    const Dfe& view = a;  // callable on a const object
    const ProbeStats scored = view.score_known(known, 3, score_fill, &ctx);
    a.snapshot_taps(after);
    CHECK(before == after);
    CHECK(scored.rows == 27);
    // Not begun: scoring still works, and a following add_training_row still refuses.
    CHECK_FALSE(a.add_training_row(std::vector<Sample>(6), std::vector<Sample>(3), Sample{}));
    CHECK(view.score_known(known, 3, score_fill, &ctx).rows == 27);
    // Same numbers as the training sweep reports for the same taps.
    b.begin_training();
    const ProbeStats trained = b.train_on_known(known, 3, score_fill, &ctx, 1.0f);
    CHECK(trained.rows == scored.rows);
    CHECK(trained.error_energy == scored.error_energy);
    CHECK(trained.correlation == scored.correlation);
    // And the error energy is what the equalise loop gives (guards against a doubled or dropped term).
    double expect = 0;
    std::vector<Sample> w(6), p(3);
    for (int i = 3; i < 30; i++) {
        score_fill(&ctx, i, w);
        for (int j = 0; j < 3; j++) p[static_cast<std::size_t>(j)] = known[static_cast<std::size_t>(i - 1 - j)];
        const Sample y = a.equalise(w, p);
        const Sample d{y.real() - known[static_cast<std::size_t>(i)].real(), y.imag() - known[static_cast<std::size_t>(i)].imag()};
        expect += static_cast<double>(d.real() * d.real() + d.imag() * d.imag());
    }
    CHECK(std::fabs(static_cast<double>(scored.error_energy) - expect) <= 1e-4 * expect);
    CHECK(view.score_known(known, 2, score_fill, &ctx).rows == 0);
}

TEST_CASE("equaliser: not a number in, false out and taps untouched", "[equaliser][nan]") {
    auto st = eq_storage(6, 3);
    Dfe d;
    REQUIRE(d.configure(6, 3, st));
    std::vector<Sample> known;
    for (int i = 0; i < 40; i++) known.push_back(Sample{static_cast<float>(i * 7 % 5) - 2.0f, static_cast<float>(i * 3 % 4) - 1.5f});
    ScoreCtx ctx{&known};
    d.begin_training();
    d.train_on_known(known, 3, score_fill, &ctx, 1.0f);
    REQUIRE(d.solve_training(1e-3f, false, 0.0f));
    std::vector<Sample> good(9), now(9);
    d.snapshot_taps(good);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Sample> w(6, Sample{1, 0}), p(3, Sample{1, 0});
    // A NaN row poisons the accumulation: the solve refuses.
    d.begin_training();
    d.train_on_known(known, 3, score_fill, &ctx, 1.0f);
    d.add_training_row(w, p, Sample{nan, 0}, 1.0f);
    CHECK_FALSE(d.solve_training(1e-3f, true, 0.0f));
    d.snapshot_taps(now);
    CHECK(now == good);
    // Same for the channel-shortening solve.
    d.begin_training();
    d.train_on_known(known, 3, score_fill, &ctx, 1.0f);
    d.add_training_row(w, p, Sample{nan, 0}, 1.0f);
    CHECK_FALSE(d.solve_training_tir(1e-3f, 0.0f, 3, true, 0).solved);
    d.snapshot_taps(now);
    CHECK(now == good);
    // A NaN sample does not reach the taps through the RLS update either.
    d.begin_rls(0.98f, 1.0f);
    d.rls_update(std::vector<Sample>(6, Sample{nan, 0}), p, Sample{1, 0}, 1.0f);
    d.snapshot_taps(now);
    CHECK(now == good);
}

TEST_CASE("equaliser: the object is not movable", "[equaliser][memory]") {
    static_assert(!std::is_move_constructible_v<Dfe>);
    static_assert(!std::is_move_assignable_v<Dfe>);
    static_assert(!std::is_copy_constructible_v<Dfe>);
    SUCCEED();
}
