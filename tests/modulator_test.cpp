// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 2 (transmit audio, issue 21): the C++ pulse shaping and up-conversion against the C#
// reference files, for every case of the standard set. The input is the burst symbol stream
// (file 09) and the probe values (file 10); the checks are the pulse taps (file 11), the baseband
// (file 12) and the audio (file 13): same length, absolute error within the documented
// tolerance, same level and same spectrum. See docs/dev/modulator.md for why the tolerance is
// what it is. Tests that need the files skip when they are absent.

#include "alloc_guard.hpp"
#include "dsp/filter_design.hpp"
#include "modulator.hpp"
#include "reference_files.hpp"
#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
using ms110d_test::ReferenceCase;

namespace {

namespace fs = std::filesystem;

// The documented tolerances (docs/reference-files.md in the C# repo, docs/dev/modulator.md here).
constexpr double audio_tolerance = 1e-5;  // absolute, per sample, baseband and audio
constexpr double pulse_tolerance = 1e-6;  // absolute, per tap

std::vector<ReferenceCase> load_cases(const fs::path& dir) {
    const auto index = ms110d_test::load_index(dir);
    std::vector<ReferenceCase> cases;
    for (const auto& name : index.cases) cases.emplace_back(dir / name, name);
    return cases;
}

// The burst symbols as constellation points. File 09 holds 8PSK indices for the preamble, 255 for
// probe symbols (values from file 10, in order) and, for data, an 8PSK index (WN 0 to 7) or a
// 16-QAM index (WN 8).
std::vector<tables::Complex32> burst_points(const ReferenceCase& c) {
    const auto& m = c.manifest();
    const auto symbols = c.bytes("burst_symbols");
    const auto probes = c.complex32("probe_values_cf32le");
    const std::size_t preamble = static_cast<std::size_t>(m.integer("preamble_symbol_count"));
    const bool qam = m.str("modulation") == "qam16";
    std::vector<tables::Complex32> out;
    out.reserve(symbols.size());
    std::size_t next_probe = 0;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const std::uint8_t v = symbols[i];
        if (v == 255) {
            REQUIRE(next_probe < probes.size());
            out.push_back(probes[next_probe++]);
        } else if (qam && i >= preamble) {
            REQUIRE(v < 16);
            out.push_back(tables::qam16[v]);
        } else {
            REQUIRE(v < 8);
            out.push_back(tables::psk8[v]);
        }
    }
    CHECK(next_probe == probes.size());
    return out;
}

TxSettings settings_of(const ReferenceCase& c) {
    const auto& m = c.manifest();
    TxSettings s;
    s.waveform_number = static_cast<int>(m.integer("wn"));
    s.constraint_length = static_cast<int>(m.integer("constraint_length"));
    s.amplitude = static_cast<float>(m.real("amplitude"));
    s.roll_off = m.real("roll_off");
    return s;
}

struct Output {
    std::vector<real> audio;
    std::vector<std::complex<real>> baseband;
};

// Modulates the whole burst through the streaming interface, reading `chunk` samples at a time.
Output modulate(Modulator& mod, const std::vector<tables::Complex32>& points, std::size_t chunk) {
    Output out;
    const std::size_t total = audio_length(points.size(), mod.taps());
    out.audio.resize(total);
    out.baseband.resize(total);
    std::size_t got = 0;
    mod.begin();
    auto drain = [&](std::size_t want) {
        while (want > 0 && got < total) {
            const std::size_t n = std::min({chunk, want, total - got});
            const std::size_t r = mod.read(std::span<real>(out.audio).subspan(got, n),
                                           std::span<std::complex<real>>(out.baseband).subspan(got, n));
            if (r == 0) break;
            got += r;
            want -= r;
        }
    };
    for (const auto& p : points) {
        REQUIRE(mod.push_symbol(p));
        drain(mod.pending());
    }
    mod.end_of_symbols();
    drain(total);
    CHECK(mod.done());
    CHECK(got == total);
    return out;
}

double rms(const std::vector<real>& v) {
    double sum = 0;
    for (real x : v) sum += static_cast<double>(x) * static_cast<double>(x);
    return std::sqrt(sum / static_cast<double>(v.size()));
}

// Magnitude of the DFT at `freq_hz` (9600 Hz sampling), by the Goertzel recurrence in double.
template <typename T>
double goertzel(const std::vector<T>& x, double freq_hz) {
    const double w = 2.0 * std::numbers::pi * freq_hz / 9600.0;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (T v : x) {
        const double s0 = static_cast<double>(v) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

// Largest difference of the magnitude spectrum, at 17 frequencies from 0 to 4800 Hz in 300 Hz
// steps, relative to the reference's strongest of them.
double spectrum_error(const std::vector<real>& a, const std::vector<float>& ref) {
    double peak = 0, worst = 0;
    for (int k = 0; k <= 16; ++k) {
        const double f = 300.0 * k;
        const double ra = goertzel(a, f);
        const double rb = goertzel(ref, f);
        peak = std::max(peak, rb);
        worst = std::max(worst, std::fabs(ra - rb));
    }
    return peak > 0 ? worst / peak : worst;
}

double max_abs_error(const std::vector<real>& a, const std::vector<float>& ref) {
    REQUIRE(a.size() == ref.size());
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(ref[i])));
    return worst;
}

double max_baseband_error(const std::vector<std::complex<real>>& a, const std::vector<std::complex<float>>& ref) {
    REQUIRE(a.size() == ref.size());
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a[i].real()) - static_cast<double>(ref[i].real())));
        worst = std::max(worst, std::fabs(static_cast<double>(a[i].imag()) - static_cast<double>(ref[i].imag())));
    }
    return worst;
}

}  // namespace

TEST_CASE("root raised cosine: limits and symmetry", "[modulator][dsp]") {
    using m0lte::dsp::root_raised_cosine;
    const double pi = std::numbers::pi;
    // t = 0: 1 - beta + 4 beta / pi.
    CHECK(root_raised_cosine(0.0, 0.35) == 1 - 0.35 + 4 * 0.35 / pi);
    // At |t| = 1 / (4 beta) the closed form is 0/0; the limit is used and must be continuous.
    const double ts = 1 / (4 * 0.35);
    CHECK(std::fabs(root_raised_cosine(ts, 0.35) - root_raised_cosine(ts + 1e-6, 0.35)) < 1e-5);
    CHECK(std::fabs(root_raised_cosine(ts, 0.35) - root_raised_cosine(ts - 1e-6, 0.35)) < 1e-5);
    for (double t : {0.25, 1.0, 2.5, 7.75}) CHECK(root_raised_cosine(t, 0.35) == root_raised_cosine(-t, 0.35));
    // Beta 0 is a sinc.
    CHECK(std::fabs(root_raised_cosine(1.5, 0.0) - std::sin(pi * 1.5) / (pi * 1.5)) < 1e-15);
}

TEST_CASE("pulse span and design", "[modulator]") {
    CHECK(pulse_span_symbols(0.35) == 16);
    CHECK(pulse_span_symbols(1.0) == 16);
    CHECK(pulse_span_symbols(0.10) == 56);
    CHECK(pulse_span_symbols(0.05) == 64);  // capped
    CHECK(pulse_span_symbols(0.04) == 0);
    CHECK(pulse_span_symbols(1.01) == 0);
    CHECK(pulse_span_symbols(std::nan("")) == 0);

    std::array<float, max_pulse_taps> pulse{};
    CHECK(design_pulse(0.35, pulse) == 65);
    CHECK(design_pulse(0.10, pulse) == 225);
    CHECK(design_pulse(0.05, pulse) == 257);
    CHECK(design_pulse(0.04, pulse) == 0);
    CHECK(design_pulse(0.35, std::span<float>(pulse).first(64)) == 0);

    for (double ro : {0.05, 0.10, 0.2, 0.35, 0.5, 1.0}) {
        INFO("roll-off " << ro);
        const std::size_t n = design_pulse(ro, pulse);
        REQUIRE(n > 0);
        double energy = 0;
        for (std::size_t i = 0; i < n; ++i) {
            energy += static_cast<double>(pulse[i]) * static_cast<double>(pulse[i]);
            CHECK(pulse[i] == pulse[n - 1 - i]);  // symmetric about the centre tap
        }
        CHECK(std::fabs(energy - 1.0) < 1e-6);  // unit energy
    }
}

TEST_CASE("configure rejects what the C# rejects", "[modulator]") {
    Modulator mod;
    CHECK_FALSE(mod.configured());
    CHECK(mod.configure(TxSettings{}));
    CHECK(mod.configured());
    CHECK(mod.taps() == 65);

    TxSettings s;
    s.waveform_number = 9;
    CHECK_FALSE(mod.configure(s));
    CHECK_FALSE(mod.configured());
    s = TxSettings{};
    s.constraint_length = 8;
    CHECK_FALSE(mod.configure(s));
    s = TxSettings{};
    s.roll_off = 0.04;
    CHECK_FALSE(mod.configure(s));
    s.roll_off = 1.01;
    CHECK_FALSE(mod.configure(s));
    s.roll_off = std::nan("");
    CHECK_FALSE(mod.configure(s));
    CHECK_FALSE(mod.push_symbol(tables::psk8[0]));  // unconfigured: refuses

    for (int wn : {0, 1, 2, 3, 4, 5, 6, 7, 8, 13}) {
        s = TxSettings{};
        s.waveform_number = wn;
        CHECK(mod.configure(s));
    }
}

TEST_CASE("modulator streaming contract", "[modulator]") {
    Modulator mod;
    REQUIRE(mod.configure(TxSettings{}));
    mod.begin();
    CHECK(mod.pending() == 0);
    CHECK(mod.read(std::span<real>{}) == 0);

    REQUIRE(mod.push_symbol(tables::psk8[0]));
    CHECK(mod.pending() == 4);
    CHECK_FALSE(mod.push_symbol(tables::psk8[0]));  // samples still unread
    std::array<real, 8> buf{};
    CHECK(mod.read(buf) == 4);  // no more than pending before the end
    CHECK(mod.pending() == 0);
    CHECK_FALSE(mod.done());

    mod.end_of_symbols();
    CHECK_FALSE(mod.push_symbol(tables::psk8[0]));
    std::size_t tail = 0;
    for (std::size_t n; (n = mod.read(buf)) != 0;) tail += n;
    CHECK(tail == mod.taps());  // the tail is exactly one pulse length
    CHECK(mod.done());

    // The first sample of a lone symbol is amplitude * pulse[0] * (1 + 0i) mixed at phase 0.
    REQUIRE(mod.configure(TxSettings{}));
    mod.begin();
    REQUIRE(mod.push_symbol(tables::psk8[0]));
    CHECK(mod.read(buf) == 4);
    CHECK_THAT(static_cast<double>(buf[0]), Catch::Matchers::WithinAbs(0.5 * static_cast<double>(mod.pulse()[0]), 1e-12));
}

TEST_CASE("modulating allocates nothing", "[modulator][alloc]") {
    Modulator mod;
    std::vector<tables::Complex32> points(300, tables::psk8[3]);
    std::vector<real> audio(audio_length(points.size(), 65));
    const ms110d_test::AllocationScope scope;
    bool ok = mod.configure(TxSettings{});
    mod.begin();
    std::size_t got = 0;
    for (const auto& p : points) {
        ok = ok && mod.push_symbol(p);
        got += mod.read(std::span<real>(audio).subspan(got));
    }
    mod.end_of_symbols();
    got += mod.read(std::span<real>(audio).subspan(got));
    const std::size_t seen = scope.allocations();
    CHECK(ok);
    CHECK(got == audio.size());
    CHECK(seen == 0);
}

TEST_CASE("reference modulator pulse taps", "[reference][modulator]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    double worst = 0;
    std::string worst_case = "all cases";
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const auto ref = c.real32("pulse_taps_f32le");
        std::array<float, max_pulse_taps> pulse{};
        const std::size_t n = design_pulse(c.manifest().real("roll_off"), pulse);
        REQUIRE(n == ref.size());
        for (std::size_t i = 0; i < n; ++i) {
            const double e = std::fabs(static_cast<double>(pulse[i]) - static_cast<double>(ref[i]));
            if (e > worst) { worst = e; worst_case = c.name(); }
            CHECK(e <= pulse_tolerance);
        }
    }
    WARN("worst pulse tap error " << worst << " in " << worst_case);
}

TEST_CASE("reference modulator audio, baseband, level and spectrum", "[reference][modulator]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    double worst_audio = 0, worst_bb = 0, worst_level = 0, worst_spectrum = 0;
    std::string case_audio, case_bb;
    std::size_t cases = 0;
    Modulator mod;
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        REQUIRE(mod.configure(settings_of(c)));
        const auto points = burst_points(c);
        const auto ref_audio = c.real32("audio_f32le");
        const auto ref_bb = c.complex32("baseband_cf32le");
        REQUIRE(ref_audio.size() == static_cast<std::size_t>(c.manifest().integer("audio_sample_count")));

        const Output out = modulate(mod, points, 4096);
        REQUIRE(out.audio.size() == ref_audio.size());  // same length
        REQUIRE(out.baseband.size() == ref_bb.size());

        const double ea = max_abs_error(out.audio, ref_audio);
        const double eb = max_baseband_error(out.baseband, ref_bb);
        if (ea > worst_audio) { worst_audio = ea; case_audio = c.name(); }
        if (eb > worst_bb) { worst_bb = eb; case_bb = c.name(); }
        CHECK(ea <= audio_tolerance);
        CHECK(eb <= audio_tolerance);

        // Level: RMS within 1e-5 relative (the sample tolerance already bounds it far tighter;
        // this catches a systematic gain error stated as a level).
        std::vector<real> ref_as_real(ref_audio.begin(), ref_audio.end());
        const double level = std::fabs(rms(out.audio) / rms(ref_as_real) - 1.0);
        worst_level = std::max(worst_level, level);
        CHECK(level <= 1e-5);

        // Spectrum: magnitude at 17 frequencies, as a fraction of the strongest.
        const double sp = spectrum_error(out.audio, ref_audio);
        worst_spectrum = std::max(worst_spectrum, sp);
        CHECK(sp <= 1e-5);
        ++cases;
    }
    CHECK(cases == 122);
    WARN("worst audio error " << worst_audio << " in " << case_audio << ", worst baseband error " << worst_bb << " in "
                              << case_bb << ", worst relative level error " << worst_level
                              << ", worst relative spectrum error " << worst_spectrum);
}

TEST_CASE("reference modulator output does not depend on the read chunk size", "[reference][modulator]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const auto index = ms110d_test::load_index(dir);
    Modulator mod;
    for (const char* name : {"wn6_short_k7_s1_p100", "wn8_short_k7_s1_p100", "wn0_short_k7_s1_p100",
                             "wn6_short_k7_s6_p100_ro0.1"}) {
        const ReferenceCase c(dir / name, name);
        INFO("case " << name);
        REQUIRE(mod.configure(settings_of(c)));
        const auto points = burst_points(c);
        const Output whole = modulate(mod, points, 100000);
        for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{64}, std::size_t{1000}}) {
            const Output part = modulate(mod, points, chunk);
            CHECK(part.audio == whole.audio);  // bit identical
            CHECK(part.baseband == whole.baseband);
        }
    }
}

TEST_CASE("mutation: the audio check fails if the signal is slightly wrong", "[reference][modulator][mutation]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const ReferenceCase c(dir / "wn6_short_k7_s1_p100", "wn6_short_k7_s1_p100");
    const auto points = burst_points(c);
    const auto ref_audio = c.real32("audio_f32le");
    const auto ref_bb = c.complex32("baseband_cf32le");
    Modulator mod;

    // Sanity: the unmutated signal passes the very same checks.
    REQUIRE(mod.configure(settings_of(c)));
    const Output good = modulate(mod, points, 4096);
    REQUIRE(max_abs_error(good.audio, ref_audio) <= audio_tolerance);
    REQUIRE(max_baseband_error(good.baseband, ref_bb) <= audio_tolerance);

    SECTION("roll-off 0.36 instead of 0.35") {
        TxSettings s = settings_of(c);
        s.roll_off = 0.36;
        REQUIRE(mod.configure(s));
        const Output bad = modulate(mod, points, 4096);
        CHECK(max_abs_error(bad.audio, ref_audio) > audio_tolerance);
        CHECK(max_baseband_error(bad.baseband, ref_bb) > audio_tolerance);
    }
    SECTION("amplitude 0.501 instead of 0.5") {
        TxSettings s = settings_of(c);
        s.amplitude = 0.501f;
        REQUIRE(mod.configure(s));
        const Output bad = modulate(mod, points, 4096);
        CHECK(max_abs_error(bad.audio, ref_audio) > audio_tolerance);
        CHECK(std::fabs(rms(bad.audio) / rms(std::vector<real>(ref_audio.begin(), ref_audio.end())) - 1.0) > 1e-5);
    }
    SECTION("carrier 1801 Hz instead of 1800 Hz") {
        // Re-mix the correct baseband at a carrier one hertz high.
        std::vector<real> bad(good.baseband.size());
        for (std::size_t i = 0; i < bad.size(); ++i) {
            const double phase = 2.0 * std::numbers::pi * 1801.0 * static_cast<double>(i) / 9600.0;
            bad[i] = static_cast<real>(static_cast<double>(good.baseband[i].real()) * std::cos(phase) -
                                       static_cast<double>(good.baseband[i].imag()) * std::sin(phase));
        }
        CHECK(max_abs_error(bad, ref_audio) > audio_tolerance);
    }
    SECTION("carrier phase off by one sample") {
        std::vector<real> bad(good.audio.size());
        const double w = 2.0 * std::numbers::pi * 3.0 / 16.0;
        for (std::size_t i = 0; i < bad.size(); ++i) {
            const double phase = w * static_cast<double>(i + 1);
            bad[i] = static_cast<real>(static_cast<double>(good.baseband[i].real()) * std::cos(phase) -
                                       static_cast<double>(good.baseband[i].imag()) * std::sin(phase));
        }
        CHECK(max_abs_error(bad, ref_audio) > audio_tolerance);
    }
}
