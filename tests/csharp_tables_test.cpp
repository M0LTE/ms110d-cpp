// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Compares every C++ table with the C# values it was ported from. The expected values are in
// tests/data/csharp-tables-0.4.1.txt, dumped from the M0LTE.Ms110d 0.4.1 package itself (the
// dump header names the commit), so this is a check against the C# code, not against the
// C++ author's reading of it. Checking the tables against the standard was done for the C#
// when it was written (double transcription) and is not repeated here. Floating point
// values are compared exactly: the C++ must transmit what the C# transmits (Base25 included,
// with its 0.309016 and 0.951056 digits; see issue 16).

#include <m0lte/ms110d/mode.hpp>

#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef MS110D_CSHARP_DUMP
#error "MS110D_CSHARP_DUMP must be defined by the build"
#endif

using namespace m0lte::ms110d;

namespace {

using Lines = std::vector<std::vector<std::string>>;

// All lines of the dump, split on spaces; '#' lines are comments.
const Lines& dump() {
    static const Lines lines = [] {
        std::ifstream in(MS110D_CSHARP_DUMP);
        Lines out;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::vector<std::string> f;
            std::string tok;
            while (ss >> tok) f.push_back(tok);
            out.push_back(f);
        }
        return out;
    }();
    return lines;
}

std::vector<std::string> entry(std::string key) {
    for (const auto& l : dump()) {
        if (l[0] == key) return l;
    }
    FAIL("missing dump entry " << key);
    return {};
}

int num(const std::string& s) {
    return std::stoi(s);
}

template <typename Table>
void check_bytes(const Table& t, const std::string& key) {
    const auto e = entry(key);
    REQUIRE(e.size() - 1 == t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        INFO(key << " entry " << i);
        CHECK(static_cast<int>(t[i]) == num(e[i + 1]));
    }
}

template <typename Table>
void check_complex(const Table& t, const std::string& key) {
    const auto e = entry(key);
    REQUIRE(e.size() - 1 == 2 * t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        INFO(key << " entry " << i);
        CHECK(t[i].real() == std::stof(e[1 + 2 * i]));
        CHECK(t[i].imag() == std::stof(e[2 + 2 * i]));
    }
}

Modulation modulation(const std::string& s) {
    if (s == "Walsh") return Modulation::Walsh;
    if (s == "Bpsk") return Modulation::Bpsk;
    if (s == "Qpsk") return Modulation::Qpsk;
    if (s == "Psk8") return Modulation::Psk8;
    if (s == "Qam16") return Modulation::Qam16;
    FAIL("unknown modulation " << s);
    return Modulation::Walsh;
}

InterleaverKind kind(const std::string& s) {
    if (s == "UltraShort") return InterleaverKind::UltraShort;
    if (s == "Short") return InterleaverKind::Short;
    if (s == "Medium") return InterleaverKind::Medium;
    if (s == "Long") return InterleaverKind::Long;
    FAIL("unknown interleaver " << s);
    return InterleaverKind::Short;
}

}  // namespace

TEST_CASE("scalar constants equal the C#", "[csharp]") {
    CHECK(tables::eom == static_cast<std::uint32_t>(std::stoul(entry("eom")[1])));
    CHECK(tables::preamble_chips_per_symbol == num(entry("preamble_chips_per_symbol")[1]));
    CHECK(tables::symbol_rate == num(entry("symbol_rate")[1]));
    CHECK(tables::sub_carrier_hz == num(entry("sub_carrier_hz")[1]));
}

TEST_CASE("PN, Walsh, di-bit and transcoding tables equal the C#", "[csharp]") {
    check_bytes(tables::fixed_pn, "fixed_pn");
    check_bytes(tables::cnt_pn, "cnt_pn");
    check_bytes(tables::wid_pn, "wid_pn");
    for (std::size_t i = 0; i < 4; ++i) check_bytes(tables::walsh[i], "walsh" + std::to_string(i));
    check_bytes(tables::fixed_dibits, "fixed_dibits");
    check_bytes(tables::transcode_bpsk, "transcode_bpsk");
    check_bytes(tables::transcode_qpsk, "transcode_qpsk");
    check_bytes(tables::transcode_8psk, "transcode_8psk");
}

TEST_CASE("constellations and mini-probe bases equal the C#", "[csharp]") {
    check_complex(tables::psk8, "psk8");
    check_complex(tables::qam16, "qam16");
    check_complex(tables::base13, "base13");
    check_complex(tables::base16, "base16");
    check_complex(tables::base19, "base19");
    check_complex(tables::base25, "base25");
}

TEST_CASE("mini-probes, base and boundary-shifted, equal the C#", "[csharp]") {
    for (int k : {24, 32, 36, 48}) {
        const tables::ProbeSpec* spec = tables::find_probe_spec(k);
        REQUIRE(spec != nullptr);
        for (bool boundary : {false, true}) {
            const auto e = entry("probe" + std::to_string(k) + (boundary ? "_boundary" : ""));
            REQUIRE(e.size() - 1 == 2 * static_cast<std::size_t>(k));
            for (int i = 0; i < k; ++i) {
                INFO("k " << k << " boundary " << boundary << " symbol " << i);
                const auto s = tables::probe_symbol(*spec, i, boundary);
                CHECK(s.real() == std::stof(e[1 + 2 * static_cast<std::size_t>(i)]));
                CHECK(s.imag() == std::stof(e[2 + 2 * static_cast<std::size_t>(i)]));
            }
        }
    }
    CHECK(tables::find_probe_spec(64) == nullptr);
    CHECK(tables::find_probe_spec(0) == nullptr);
}

TEST_CASE("modes and interleavers equal the C#", "[csharp]") {
    int modes = 0;
    int interleavers = 0;
    for (const auto& l : dump()) {
        if (l[0] == "mode") {
            const int wn = num(l[1]);
            INFO("WN " << wn);
            const auto m = mode_3k(wn);
            REQUIRE(m.has_value());
            CHECK(m->wn == wn);
            CHECK(m->modulation == modulation(l[2]));
            CHECK(m->bits_per_symbol == num(l[3]));
            CHECK(m->u == num(l[4]));
            CHECK(m->k == num(l[5]));
            const auto slash = l[6].find('/');
            CHECK(m->code_rate == CodeRate{num(l[6].substr(0, slash)), num(l[6].substr(slash + 1))});
            CHECK(m->bps == num(l[7]));
            ++modes;
        } else if (l[0] == "il") {
            const int wn = num(l[1]);
            INFO("WN " << wn << " " << l[2]);
            const auto p = interleaver_3k(wn, kind(l[2]));
            CHECK(has_interleaver_3k(wn, kind(l[2])) == (l[3] != "none"));
            if (l[3] == "none") {
                CHECK_FALSE(p.has_value());
            } else {
                REQUIRE(p.has_value());
                CHECK(p->frames == num(l[3]));
                CHECK(p->size_bits == num(l[4]));
                CHECK(p->input_bits == num(l[5]));
                CHECK(p->increment == num(l[6]));
            }
            ++interleavers;
        } else if (l[0] == "nomode") {
            CHECK_FALSE(mode_3k(num(l[1])).has_value());
            CHECK_FALSE(has_interleaver_3k(num(l[1]), InterleaverKind::Short));
        }
    }
    CHECK(modes == 10);
    CHECK(interleavers == 40);
    CHECK(modes_3k().size() == 10);
    CHECK_FALSE(mode_3k(-1).has_value());
    CHECK_FALSE(mode_3k(15).has_value());
}

TEST_CASE("mode and interleaver tables are consistent with each other", "[csharp]") {
    const InterleaverKind kinds[] = {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium,
                                     InterleaverKind::Long};
    for (const Mode& m : modes_3k()) {
        for (InterleaverKind k : kinds) {
            const auto p = interleaver_3k(m.wn, k);
            if (!p) continue;
            INFO("WN " << m.wn << " interleaver " << static_cast<int>(k));
            // The coded and punctured block fits the interleaver exactly (D.5.3.2).
            CHECK(p->input_bits * m.code_rate.denominator == p->size_bits * m.code_rate.numerator);
            // Frames of U symbols at bits_per_symbol fill the interleaver (WN 0: frames are the
            // Walsh channel symbols, two bits each).
            const int per_frame = m.wn == 0 ? 1 : m.u;
            CHECK(p->frames * per_frame * m.bits_per_symbol == p->size_bits);
        }
        if (m.wn != 0) {
            // User rate = symbol rate x U/(U+K) x bits per symbol x code rate.
            const long long num_ = 2400LL * m.u * m.bits_per_symbol * m.code_rate.numerator;
            const long long den = static_cast<long long>(m.u + m.k) * m.code_rate.denominator;
            CHECK(num_ == m.bps * den);
        }
    }
}
