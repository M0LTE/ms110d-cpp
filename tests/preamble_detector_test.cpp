// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The preamble detector (issue 24) against the C# demodulator's own preamble search, whose
// answers are committed in tests/data/csharp-detect-0.2.1.txt (tools/csharp-vectors, detect
// mode): every reference case, seeded impaired scenarios (carrier offset, fractional delay,
// noise) and noise-only streams for false alarms. Plus the transmitted parameters, chunking,
// state handling and that nothing allocates.

#include <m0lte/ms110d/transmitter.hpp>

#include "alloc_guard.hpp"
#include "dsp/fir_filter.hpp"
#include "impair.hpp"
#include "framing.hpp"
#include "modulator.hpp"
#include "preamble_detector.hpp"
#include "tables.hpp"
#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
using ms110d_test::DumpLine;

namespace {

namespace fs = std::filesystem;

struct Run {
    bool det = false;
    PreambleDetection e;
    PreambleSearchStats stats;
    std::int64_t t2 = 0;
};

Run run_detector(PreambleDetector& d, const std::vector<float>& audio, double threshold, std::size_t chunk) {
    PreambleDetectorConfig cfg;
    cfg.sync_threshold = threshold;
    REQUIRE(d.configure(cfg));
    Run r;
    std::size_t at = 0;
    while (at < audio.size() && !r.det) {
        const std::size_t n = std::min(chunk, audio.size() - at);
        at += d.process(std::span<const float>(audio).subspan(at, n));
        if (d.has_detection()) {
            r.det = d.take_detection(r.e);
        }
    }
    r.stats = d.stats();
    r.t2 = d.t2_written();
    return r;
}

struct Agreement {
    int lines = 0;
    int same_decision = 0;
    int both_detected = 0;
    int mismatches = 0;
    double max_data_t2 = 0;
    double max_cfo = 0;
    double max_omega = 0;
    double max_theta = 0;
    int agc_engaged = 0;
};

// Compares a run with a dump line; returns true if every field agrees. Tolerances are stated in
// docs/dev/preamble-detector.md: the integers and the decision exactly, the data start to 1e-6 T/2, the frequency to 1e-4 Hz.
bool agrees(const DumpLine& l, const Run& r, Agreement& a, std::string& why) {
    ++a.lines;
    const bool cs_det = l.integer(l.result, "det") != 0;
    if (cs_det != r.det) {
        why = "decision differs";
        ++a.mismatches;
        return false;
    }
    ++a.same_decision;
    bool ok = true;
    if ((r.stats.locks > 0) != (l.integer(l.result, "locked") != 0)) { why += " locked"; ok = false; }
    if (r.stats.peaks_accepted != l.integer(l.result, "peaks")) { why += " peaks"; ok = false; }
    const std::int64_t rej = r.stats.rejected_count + r.stats.rejected_margin + r.stats.rejected_wid;
    if (rej != l.integer(l.result, "rejects")) { why += " rejects"; ok = false; }
    if (std::abs(r.stats.peak_search_metric - l.real(l.result, "peakmetric")) > 1e-9) { why += " peakmetric"; ok = false; }
    if (cs_det) {
        ++a.both_detected;
        if (r.e.waveform != l.integer(l.result, "wn")) { why += " wn"; ok = false; }
        if (static_cast<int>(r.e.interleaver) != l.integer(l.result, "il")) { why += " il"; ok = false; }
        if (r.e.constraint_length != l.integer(l.result, "k")) { why += " k"; ok = false; }
        if (r.e.superframes_after != l.integer(l.result, "count")) { why += " count"; ok = false; }
        if (r.e.locked_at_sample != l.integer(l.result, "at")) { why += " at"; ok = false; }
        if (r.e.detected_at_sample < r.e.locked_at_sample) { why += " order"; ok = false; }
        if (r.e.locked_at_sample / 2 != l.integer(l.result, "t2")) { why += " t2"; ok = false; }
        if (l.real(l.result, "tau") != 0) { why += " tau"; ok = false; }
        const double dd = std::abs(r.e.data_start_t2 - l.real(l.result, "data"));
        const double dc = std::abs(r.e.cfo_hz - l.real(l.result, "cfo"));
        const double dw = std::abs(r.e.carrier_omega - l.real(l.result, "omega"));
        const double dt = std::abs(r.e.carrier_theta_base - l.real(l.result, "theta"));
        a.max_data_t2 = std::max(a.max_data_t2, dd);
        a.max_cfo = std::max(a.max_cfo, dc);
        a.max_omega = std::max(a.max_omega, dw);
        a.max_theta = std::max(a.max_theta, dt);
        if (std::abs(r.e.chip0_t2 - l.real(l.result, "chip0")) > 1e-9) { why += " chip0"; ok = false; }
        if (std::abs(r.e.cfo_hz_first_fit - l.real(l.result, "cfo0")) > 1e-4) { why += " cfo0"; ok = false; }
        if (std::abs(r.e.carrier_omega_first_fit - l.real(l.result, "omega0")) > 1e-8) { why += " omega0"; ok = false; }
        if (std::abs(r.e.carrier_theta_base_first_fit - l.real(l.result, "theta0")) > 1e-5) { why += " theta0"; ok = false; }
        if (std::abs(static_cast<double>(r.e.agc_gain) - l.real(l.result, "agc")) > 1e-4 * l.real(l.result, "agc")) { why += " agc"; ok = false; }
        if (r.e.agc_gain != 1.0f) ++a.agc_engaged;
        if (dd > 1e-6) { why += " data"; ok = false; }
        if (dc > 1e-4) { why += " cfo"; ok = false; }
        if (dw > 1e-8) { why += " omega"; ok = false; }
        if (dt > 1e-5) { why += " theta"; ok = false; }
    }
    if (!ok) ++a.mismatches;
    return ok;
}

void report(const char* what, const Agreement& a) {
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "%s: %d lines, decision agrees %d, detected by both %d, level gain engaged %d, max diff data start %.3g T/2, cfo %.3g Hz, omega %.3g, theta %.3g",
                  what, a.lines, a.same_decision, a.both_detected, a.agc_engaged, a.max_data_t2, a.max_cfo, a.max_omega, a.max_theta);
    WARN(buf);
}

std::vector<DumpLine> dump_lines(const std::string& kind) {
    std::vector<DumpLine> out;
    for (auto& l : ms110d_test::load_dump(MS110D_CSHARP_DETECT_DUMP)) {
        if (l.kind == kind) out.push_back(std::move(l));
    }
    return out;
}

InterleaverKind kind_from_name(const std::string& s) {
    if (s == "ultrashort") return InterleaverKind::UltraShort;
    if (s == "short") return InterleaverKind::Short;
    if (s == "medium") return InterleaverKind::Medium;
    return InterleaverKind::Long;
}

auto make_detector() { return std::make_unique<PreambleDetector>(); }

}  // namespace

TEST_CASE("reference preamble detector: every reference case agrees with the C# and the transmitter", "[preamble-detector][reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    auto det = make_detector();
    Agreement a;
    int undetected = 0;
    for (const DumpLine& l : dump_lines("ref")) {
        INFO(l.name);
        const ms110d_test::ReferenceCase c(dir / l.name, l.name);
        const auto audio = c.real32("audio_f32le");
        const Run r = run_detector(*det, audio, 0.32, 4800);
        std::string why;
        CHECK(agrees(l, r, a, why));
        CHECK(why.empty());
        const auto& m = c.manifest();
        if (r.det) {
            // What was transmitted: the waveform, the interleaver, K, the downcount, and the data start
            // (the first data symbol's pulse is centred at tx sample 4 * preamble symbols + the pulse delay).
            CHECK(r.e.waveform == m.integer("wn"));
            CHECK(r.e.interleaver == kind_from_name(m.str("interleaver")));
            CHECK(r.e.constraint_length == m.integer("constraint_length"));
            CHECK(r.e.superframes_after == m.integer("preamble_superframes") - 1);
            const double expected = 4.0 * static_cast<double>(m.integer("preamble_symbol_count")) +
                                    static_cast<double>(m.integer("pulse_tap_count") - 1) / 2.0;
            CHECK(std::abs(r.e.data_start_sample - expected) < 0.5);
        } else {
            ++undetected;
            // Only the single-super-frame preamble, which the C# does not acquire either, and the
            // 0.10 roll-off case may go unseen; anything else is a failure to detect.
            const bool excused = m.integer("preamble_superframes") < 2 || m.real("roll_off") != 0.35;
            CHECK(excused);
        }
    }
    report("reference cases", a);
    CHECK(a.lines == 122);
    CHECK(undetected <= 2);
}

TEST_CASE("reference preamble detector: the result does not depend on the chunk size", "[preamble-detector][reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    auto det = make_detector();
    for (const char* name : {"wn0_short_k7_s1_p100", "wn4_medium_k7_s1_p100", "wn8_short_k7_s1_p100"}) {
        INFO(name);
        const ms110d_test::ReferenceCase c(dir / name, name);
        const auto audio = c.real32("audio_f32le");
        const Run whole = run_detector(*det, audio, 0.32, audio.size());
        REQUIRE(whole.det);
        for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{480}, std::size_t{4097}}) {
            const Run r = run_detector(*det, audio, 0.32, chunk);
            REQUIRE(r.det);
            CHECK(r.e.data_start_t2 == whole.e.data_start_t2);
            CHECK(r.e.cfo_hz == whole.e.cfo_hz);
            CHECK(r.e.carrier_omega == whole.e.carrier_omega);
            CHECK(r.e.detected_at_sample == whole.e.detected_at_sample);
        }
    }
}

TEST_CASE("reference preamble detector: impaired scenarios agree with the C#", "[preamble-detector][reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    auto det = make_detector();
    Agreement a;
    std::map<int, std::pair<int, int>> by_snr;  // snr -> {C# detections, C++ detections} at threshold 0.32
    int shown = 0;
    for (const DumpLine& l : dump_lines("scn")) {
        const auto s = ms110d_test::scenario_of(l);
        const auto audio = ms110d_test::build_scenario(dir, s);
        const Run r = run_detector(*det, audio, s.threshold, 3000);
        std::string why;
        const bool ok = agrees(l, r, a, why);
        if (!ok && shown++ < 10) {
            INFO(l.name << " snr=" << s.snr_db << " df=" << s.df_hz << " delay=" << s.delay << " lead=" << s.lead);
            CHECK(why.empty());
        }
        if (s.threshold == 0.32) {
            auto& p = by_snr[static_cast<int>(s.snr_db)];
            p.first += l.integer(l.result, "det") != 0;
            p.second += r.det;
        }
    }
    report("impaired scenarios", a);
    for (const auto& [snr, p] : by_snr) {
        char buf[100];
        std::snprintf(buf, sizeof buf, "SNR %d dB (3 kHz): detections C# %d, C++ %d", snr, p.first, p.second);
        WARN(buf);
        CHECK(p.first == p.second);
    }
    CHECK(a.mismatches == 0);
    CHECK(a.lines > 300);
    CHECK(a.agc_engaged >= 10);  // the weak signal scenarios do reach the level gain
}

TEST_CASE("preamble detector: false alarms on noise are the C#'s", "[preamble-detector]") {
    auto det = make_detector();
    Agreement a;
    long long cs_peaks = 0, cpp_peaks = 0, cs_det = 0, cpp_det = 0;
    for (const DumpLine& l : dump_lines("noise")) {
        const auto audio = ms110d_test::noise_only(static_cast<std::uint64_t>(std::stoull(l.params.at("seed"))),
                                                   static_cast<int>(l.integer(l.params, "samples")), l.real(l.params, "sigma"));
        const Run r = run_detector(*det, audio, l.real(l.params, "thr"), 9600);
        std::string why;
        CHECK(agrees(l, r, a, why));
        cs_peaks += l.integer(l.result, "peaks");
        cpp_peaks += r.stats.peaks_accepted;
        cs_det += l.integer(l.result, "det");
        cpp_det += r.det;
    }
    char buf[200];
    std::snprintf(buf, sizeof buf, "noise streams: %d, accepted peaks C# %lld C++ %lld, false detections C# %lld C++ %lld", a.lines,
                  cs_peaks, cpp_peaks, cs_det, cpp_det);
    WARN(buf);
    CHECK(cpp_det <= cs_det);
    CHECK(cpp_peaks <= cs_peaks);
    CHECK(a.mismatches == 0);
}

TEST_CASE("preamble detector: transmitter audio is identified at every waveform without the reference files", "[preamble-detector]") {
    auto det = make_detector();
    for (const int wn : {0, 1, 2, 3, 4, 5, 6, 7, 8, 13}) {
        TxSettings s;
        s.waveform_number = wn;
        s.interleaver = wn == 0 ? InterleaverKind::Short : InterleaverKind::UltraShort;
        s.constraint_length = wn % 2 ? 9 : 7;
        const std::vector<std::uint8_t> bits(100, 1);
        std::size_t bytes = 0;
        REQUIRE(Transmitter::storage_bytes(s, bits.size(), bytes) == Status::Ok);
        std::vector<std::uint8_t> mem(bytes);
        Transmitter tx;
        REQUIRE(tx.configure(s, bits.size(), mem) == Status::Ok);
        std::size_t n = 0;
        REQUIRE(tx.audio_samples(bits.size(), n) == Status::Ok);
        std::vector<float> audio(n);
        REQUIRE(tx.modulate(bits, audio, n) == Status::Ok);
        // 500 samples of lead-in, so the detector sees silence first.
        audio.insert(audio.begin(), 500, 0.0f);
        const Run r = run_detector(*det, audio, 0.32, 1000);
        INFO("wn " << wn);
        REQUIRE(r.det);
        CHECK(r.e.waveform == wn);
        CHECK(r.e.interleaver == s.interleaver);
        CHECK(r.e.constraint_length == s.constraint_length);
        CHECK(r.e.superframes_after == 2);
        CHECK(std::abs(r.e.data_start_sample - (500.0 + 4.0 * 1728.0 + 32.0)) < 0.5);
        CHECK(std::abs(r.e.cfo_hz) < 0.01);
    }
}

TEST_CASE("preamble detector: a long preamble waits for the data start before the event", "[preamble-detector]") {
    TxSettings s;
    s.waveform_number = 0;
    s.preamble_superframes = 9;
    const std::vector<std::uint8_t> bits(40, 1);
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, bits.size(), bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(s, bits.size(), mem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(bits.size(), n) == Status::Ok);
    std::vector<float> audio(n);
    REQUIRE(tx.modulate(bits, audio, n) == Status::Ok);
    auto det = make_detector();
    const Run r = run_detector(*det, audio, 0.32, 960);
    REQUIRE(r.det);
    CHECK(r.e.superframes_after == 8);
    CHECK(r.e.locked_at_sample < r.e.detected_at_sample);
    const double data = 4.0 * 9 * 576 + 32.0;
    CHECK(std::abs(r.e.data_start_sample - data) < 0.5);
    // The event completes just after the data start (the samples the re-fit needs).
    CHECK(static_cast<double>(r.e.detected_at_sample) > data - 32.0);
    CHECK(static_cast<double>(r.e.detected_at_sample) < data + 200.0);
    CHECK(std::abs(r.e.cfo_hz) < 0.01);
}

TEST_CASE("preamble detector: state handling", "[preamble-detector]") {
    auto det = make_detector();
    PreambleDetectorConfig bad;
    bad.roll_off = 0.01;
    CHECK_FALSE(det->configure(bad));
    CHECK_FALSE(det->configured());
    CHECK(det->process(std::vector<float>(100, 0.0f)) == 0);
    REQUIRE(det->configure(PreambleDetectorConfig{}));
    CHECK(det->searching());

    // Silence never detects, and every sample is used.
    const std::vector<float> silence(5000, 0.0f);
    CHECK(det->process(silence) == silence.size());
    CHECK_FALSE(det->has_detection());
    PreambleDetection e;
    CHECK_FALSE(det->take_detection(e));
    CHECK(det->t2_written() == 2500);
    CHECK(det->t2_available(2499));
    CHECK_FALSE(det->t2_available(2500));
}

TEST_CASE("preamble detector: detection halts input, then resumes", "[preamble-detector]") {
    TxSettings s;
    s.waveform_number = 1;
    const std::vector<std::uint8_t> bits(50, 0);
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, bits.size(), bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(s, bits.size(), mem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(bits.size(), n) == Status::Ok);
    std::vector<float> audio(n);
    REQUIRE(tx.modulate(bits, audio, n) == Status::Ok);

    auto det = make_detector();
    const std::size_t used = det->process(audio);
    CHECK(used < audio.size());
    CHECK(det->has_detection());
    CHECK(det->process(std::span<const float>(audio).subspan(used)) == 0);  // nothing until it is taken
    PreambleDetection e;
    REQUIRE(det->take_detection(e));
    CHECK(static_cast<std::size_t>(e.detected_at_sample) == used);
    CHECK_FALSE(det->take_detection(e));
    // Idle: the front end carries on, the search does not run.
    const std::size_t rest = audio.size() - used;
    CHECK(det->process(std::span<const float>(audio).subspan(used)) == rest);
    CHECK_FALSE(det->has_detection());
    CHECK(det->stats().detections == 1);
    // The T/2 stream stays readable for the demodulator.
    CHECK(det->t2_available(det->t2_written() - 1));
    CHECK_FALSE(det->t2_available(det->t2_written() - static_cast<std::int64_t>(PreambleDetector::ring_size) - 1));
    det->resume_search();
    CHECK(det->searching());
}

TEST_CASE("preamble detector: nothing allocates after construction", "[preamble-detector][alloc]") {
    TxSettings s;
    s.waveform_number = 4;
    const std::vector<std::uint8_t> bits(80, 1);
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, bits.size(), bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(s, bits.size(), mem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(bits.size(), n) == Status::Ok);
    std::vector<float> audio(n);
    REQUIRE(tx.modulate(bits, audio, n) == Status::Ok);
    std::vector<float> noise = ms110d_test::noise_only(7, 30000, 0.1);

    auto det = make_detector();
    PreambleDetection e;
    bool got = false;
    std::size_t used = 0;
    const ms110d_test::AllocationScope scope;
    det->process(noise);
    det->reset();
    used = det->process(audio);
    got = det->take_detection(e);
    det->resume_search();
    PreambleDetectorConfig cfg;
    const bool configured = det->configure(cfg);
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(got);
    CHECK(configured);
    CHECK(used > 0);
}

TEST_CASE("fir filter: matches a direct convolution and next() equals push() then output()", "[preamble-detector][dsp]") {
    const float coeffs[5] = {0.5f, -0.25f, 0.125f, 0.75f, -1.0f};
    m0lte::dsp::FirFilter<8> f;
    m0lte::dsp::FirFilter<8> g;
    CHECK_FALSE(f.configure({}));
    REQUIRE(f.configure(coeffs));
    REQUIRE(g.configure(coeffs));
    std::vector<float> x;
    for (int i = 0; i < 40; ++i) x.push_back(static_cast<float>(std::sin(0.37 * i)) + 0.01f * static_cast<float>(i));
    for (std::size_t n = 0; n < x.size(); ++n) {
        float expect = 0;
        for (std::size_t k = 0; k < 5 && k <= n; ++k) expect += coeffs[k] * x[n - k];
        const float y = f.next(x[n]);
        g.push(x[n]);
        CHECK(std::abs(y - expect) < 1e-6f);
        CHECK(g.output() == y);
    }
}

namespace {

// A bare preamble of `m` super-frames with the given waveform ID di-bits (which need not be a
// valid ID), modulated by the real modulator, with silence either side.
std::vector<float> preamble_audio(const std::array<std::uint8_t, 5>& wid, int m) {
    Modulator mod;
    REQUIRE(mod.configure(TxSettings{}));
    mod.begin();
    std::vector<float> audio(500, 0.0f);
    std::vector<real> chunk(4);
    const auto push = [&](std::uint8_t chip) {
        REQUIRE(mod.push_symbol(tables::psk8[chip]));
        const std::size_t got = mod.read(chunk);
        for (std::size_t i = 0; i < got; ++i) audio.push_back(static_cast<float>(chunk[i]));
    };
    for (int rep = 0; rep < m; ++rep) {
        const auto count = encode_count(m - 1 - rep);
        for (std::size_t i = 0; i < 288; ++i) {
            push(static_cast<std::uint8_t>((tables::walsh[tables::fixed_dibits[i / 32]][i & 3] + tables::fixed_pn[i % 256]) & 7));
        }
        for (std::size_t i = 0; i < 128; ++i) {
            push(static_cast<std::uint8_t>((tables::walsh[(*count)[i / 32]][i & 3] + tables::cnt_pn[i]) & 7));
        }
        for (std::size_t i = 0; i < 160; ++i) {
            push(static_cast<std::uint8_t>((tables::walsh[wid[i / 32]][i & 3] + tables::wid_pn[i]) & 7));
        }
    }
    mod.end_of_symbols();
    for (;;) {
        const std::size_t got = mod.read(chunk);
        if (got == 0) break;
        for (std::size_t i = 0; i < got; ++i) audio.push_back(static_cast<float>(chunk[i]));
    }
    audio.insert(audio.end(), 500, 0.0f);
    return audio;
}

}  // namespace

TEST_CASE("preamble detector: the waveform ID check bits and defined pairs are enforced", "[preamble-detector]") {
    auto det = make_detector();
    const auto good = *encode_wid(1, InterleaverKind::Short, 7);

    {
        const Run r = run_detector(*det, preamble_audio(good, 3), 0.32, 960);
        REQUIRE(r.det);
        CHECK(r.e.waveform == 1);
        CHECK(r.e.interleaver == InterleaverKind::Short);
        CHECK(r.stats.rejected_wid == 0);
    }

    // Each of the three check bits d2 (in di-bit 3) and d1, d0 (in di-bit 4) flipped in turn, and
    // the constraint length bit d3 (which the check covers): all rejected on the checksum.
    struct Flip { const char* name; std::size_t dibit; std::uint8_t mask; };
    for (const Flip f : {Flip{"d2", 3, 1}, Flip{"d1", 4, 2}, Flip{"d0", 4, 1}, Flip{"d3", 3, 2}}) {
        INFO(f.name);
        auto bad = good;
        bad[f.dibit] = static_cast<std::uint8_t>(bad[f.dibit] ^ f.mask);
        const Run r = run_detector(*det, preamble_audio(bad, 3), 0.32, 960);
        CHECK_FALSE(r.det);
        CHECK(r.stats.peaks_accepted >= 1);
        CHECK(r.stats.rejected_wid >= 1);
        CHECK(r.stats.locks == 0);
    }

    // A valid checksum on a pair the tables do not define (WN 0 has no ultra-short row) is
    // rejected too, and so are a waveform number the library does not implement (WN 9) and a reserved one (WN 14).
    for (const auto& wid : {*encode_wid(0, InterleaverKind::UltraShort, 7), *encode_wid(9, InterleaverKind::Short, 7),
                            std::array<std::uint8_t, 5>{3, 2, 1, 1, 3}}) {  // the last: WN 14, reserved, with a valid checksum
        const Run r = run_detector(*det, preamble_audio(wid, 3), 0.32, 960);
        CHECK_FALSE(r.det);
        CHECK(r.stats.rejected_wid >= 1);
    }
}

TEST_CASE("preamble detector: a default constructed detector detects at the default threshold", "[preamble-detector]") {
    CHECK(PreambleDetectorConfig{}.sync_threshold == 0.32);
    TxSettings s;
    s.waveform_number = 3;
    s.interleaver = InterleaverKind::Medium;
    const std::vector<std::uint8_t> bits(60, 1);
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, bits.size(), bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(s, bits.size(), mem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(bits.size(), n) == Status::Ok);
    std::vector<float> audio(n);
    REQUIRE(tx.modulate(bits, audio, n) == Status::Ok);

    PreambleDetector det;  // no configure call
    CHECK(det.configured());
    std::size_t at = 0;
    while (at < audio.size() && !det.has_detection()) at += det.process(std::span<const float>(audio).subspan(at));
    PreambleDetection e;
    REQUIRE(det.take_detection(e));
    CHECK(e.waveform == 3);
    CHECK(e.interleaver == InterleaverKind::Medium);
    CHECK(e.agc_gain == 1.0f);
    CHECK(e.input_level > 0.04);
}
