// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Links the C++ tables to the C# reference files (docs/dev/reference-files.md): every case's
// manifest must agree with the tables, and the table-driven stages (transcoding, probes,
// the preamble's Fixed section, the EOM) must reproduce the files bit for bit. Skipped when
// the reference files have not been generated.

#include <m0lte/ms110d/mode.hpp>

#include "reference_files.hpp"
#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
using ms110d_test::Manifest;
using ms110d_test::ReferenceCase;

namespace {

namespace fs = std::filesystem;

InterleaverKind kind_from_name(const std::string& s) {
    if (s == "ultrashort") return InterleaverKind::UltraShort;
    if (s == "short") return InterleaverKind::Short;
    if (s == "medium") return InterleaverKind::Medium;
    if (s == "long") return InterleaverKind::Long;
    FAIL("unknown interleaver " << s);
    return InterleaverKind::Short;
}

const char* modulation_name(Modulation m) {
    switch (m) {
        case Modulation::Walsh: return "walsh";
        case Modulation::Bpsk: return "bpsk";
        case Modulation::Qpsk: return "qpsk";
        case Modulation::Psk8: return "psk8";
        case Modulation::Qam16: return "qam16";
    }
    return "";
}

std::string rate_name(CodeRate r) {
    return std::to_string(r.numerator) + "/" + std::to_string(r.denominator);
}

std::vector<ReferenceCase> load_cases(const fs::path& dir) {
    const auto index = ms110d_test::load_index(dir);
    std::vector<ReferenceCase> cases;
    for (const auto& name : index.cases) cases.emplace_back(dir / name, name);
    return cases;
}

std::string pin_value(const std::string& key) {
    std::ifstream in(MS110D_PIN_FILE);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return {};
}

}  // namespace

TEST_CASE("reference reader loads the index and every case", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const auto index = ms110d_test::load_index(dir);
    CHECK(index.format_version == 2);
    CHECK(index.package_version == pin_value("nuget_version"));
    CHECK(index.git_commit == pin_value("git_sha"));
    CHECK_FALSE(index.dirty);
    CHECK(index.cases.size() == 122);

    for (const auto& name : index.cases) {
        INFO("case " << name);
        const ReferenceCase c(dir / name, name);
        const Manifest& m = c.manifest();
        CHECK(m.str("case") == name);
        CHECK(m.integer("format_version") == 2);
        // Small stages are read in full, with the size checked against the manifest count.
        for (const char* stage :
             {"payload_bits", "framed_bits", "encoded_bits", "punctured_bits", "interleaved_bits", "preamble_symbols",
              "transcoded_symbols", "scramble_symbols", "data_symbols", "burst_symbols"}) {
            CHECK(c.bytes(stage).size() == static_cast<std::size_t>(m.integer(std::string("count.") + stage)));
        }
        CHECK(c.complex32("probe_values_cf32le").size() ==
              static_cast<std::size_t>(m.integer("count.probe_values_cf32le")));
        CHECK(c.real32("pulse_taps_f32le").size() == static_cast<std::size_t>(m.integer("pulse_tap_count")));
        // The audio files are large; check the size on disk only.
        CHECK(fs::file_size(c.dir() / m.str("file.audio_f32le")) ==
              4u * static_cast<std::uintmax_t>(m.integer("audio_sample_count")));
        CHECK(fs::file_size(c.dir() / m.str("file.baseband_cf32le")) ==
              8u * static_cast<std::uintmax_t>(m.integer("audio_sample_count")));
    }
}

TEST_CASE("reference cases agree with the mode and interleaver tables", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const Manifest& m = c.manifest();
        const int wn = static_cast<int>(m.integer("wn"));
        const auto mode = mode_3k(wn);
        REQUIRE(mode.has_value());
        const InterleaverKind kind = kind_from_name(m.str("interleaver"));
        const auto il = interleaver_3k(wn, kind);
        REQUIRE(il.has_value());

        CHECK(m.str("modulation") == modulation_name(mode->modulation));
        CHECK(m.str("code_rate") == rate_name(mode->code_rate));
        const long long k_constraint = m.integer("constraint_length");
        CHECK((k_constraint == 7 || k_constraint == 9));
        CHECK(m.integer("sample_rate") == 4 * tables::symbol_rate);

        CHECK(m.integer("block_input_bits") == il->input_bits);
        CHECK(m.integer("block_interleaver_bits") == il->size_bits);
        const long long blocks = m.integer("blocks");
        const long long preamble = m.integer("preamble_symbol_count");
        const long long superframes = m.integer("preamble_superframes");
        const long long tlc = m.integer("tlc_blocks");
        // A super-frame is 18 Walsh symbols of 32 chips, or 10 when M = 1 (D.5.2.1.3).
        CHECK(preamble == tables::preamble_chips_per_symbol * (tlc + superframes * (superframes == 1 ? 10 : 18)));

        if (wn == 0) {
            CHECK(m.integer("frames_per_block") == 0);
            CHECK(m.integer("walsh_symbols_per_block") == il->frames);
            CHECK(m.integer("frame_data_symbols") == 0);
            CHECK(m.integer("probe_symbols") == 0);
            CHECK(m.integer("initial_probe_symbols") == 0);
            CHECK(m.integer("eot_extension_symbols") == 0);
            // 32 chips per Walsh channel symbol.
            const long long data = blocks * il->frames * tables::preamble_chips_per_symbol;
            CHECK(m.integer("data_symbol_count") == data);
            CHECK(m.integer("burst_symbol_count") == preamble + data);
        } else {
            CHECK(m.integer("frames_per_block") == il->frames);
            CHECK(m.integer("walsh_symbols_per_block") == 0);
            CHECK(m.integer("frame_data_symbols") == mode->u);
            CHECK(m.integer("probe_symbols") == mode->k);
            CHECK(m.integer("initial_probe_symbols") == mode->k);
            const long long eot = m.integer("append_eot") != 0 ? 32 : 0;
            CHECK(m.integer("eot_extension_symbols") == eot);
            const long long data = blocks * il->frames * mode->u;
            CHECK(m.integer("data_symbol_count") == data);
            CHECK(m.integer("burst_symbol_count") == preamble + mode->k + blocks * il->frames * (mode->u + mode->k) + eot);
            CHECK(il->frames * mode->u * mode->bits_per_symbol == il->size_bits);
        }
    }
}

TEST_CASE("reference EOM bits are the EOM word", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        const Manifest& m = c.manifest();
        if (m.integer("append_eom") == 0) continue;
        INFO("case " << c.name());
        const auto framed = c.bytes("framed_bits");
        const auto payload = static_cast<std::size_t>(m.integer("payload_bits"));
        REQUIRE(framed.size() >= payload + 32);
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < 32; ++i) word = (word << 1) | framed[payload + i];
        CHECK(word == tables::eom);
        ++checked;
    }
    CHECK(checked > 100);
}

TEST_CASE("reference data symbols follow the transcoding tables", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        const Manifest& m = c.manifest();
        const int wn = static_cast<int>(m.integer("wn"));
        if (wn == 0) continue;  // WN 0 files hold Walsh chips, not transcoded symbols
        INFO("case " << c.name());
        const auto mode = mode_3k(wn);
        const auto bits = c.bytes("interleaved_bits");
        const auto symbols = c.bytes("transcoded_symbols");
        const auto bps = static_cast<std::size_t>(mode->bits_per_symbol);
        REQUIRE(bits.size() == symbols.size() * bps);
        for (std::size_t n = 0; n < symbols.size(); ++n) {
            unsigned value = 0;
            for (std::size_t b = 0; b < bps; ++b) value = (value << 1) | bits[n * bps + b];
            unsigned expected = 0;
            switch (mode->modulation) {
                case Modulation::Bpsk: expected = tables::transcode_bpsk[value]; break;
                case Modulation::Qpsk: expected = tables::transcode_qpsk[value]; break;
                case Modulation::Psk8: expected = tables::transcode_8psk[value]; break;
                case Modulation::Qam16: expected = value; break;  // the four bits are the symbol
                case Modulation::Walsh: break;
            }
            REQUIRE(symbols[n] == expected);
        }
        ++checked;
    }
    CHECK(checked > 100);
}

TEST_CASE("reference mini-probes follow the probe tables and the boundary rule", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    int cases_with_shift = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        const Manifest& m = c.manifest();
        const int wn = static_cast<int>(m.integer("wn"));
        if (wn == 0) continue;
        INFO("case " << c.name());
        const auto mode = mode_3k(wn);
        const auto il = interleaver_3k(wn, kind_from_name(m.str("interleaver")));
        const tables::ProbeSpec* spec = tables::find_probe_spec(mode->k);
        REQUIRE(spec != nullptr);

        const auto probes = c.complex32("probe_values_cf32le");
        const long long blocks = m.integer("blocks");
        const long long eot = m.integer("eot_extension_symbols");
        const int k = mode->k;

        // Expected probe values in burst order: the initial probe unshifted, then one probe
        // after each frame, shifted by the D.5.2.2 rule (the C# since 0.3.0): the probe after data frame g,
        // counted from 0 over the burst, when g % L == L - 2, L the Long interleaver's frames, whatever
        // interleaver the case uses. Written out here, not taken from the library.
        const auto long_il = interleaver_3k(wn, InterleaverKind::Long);
        int shifts = 0;
        std::vector<std::complex<float>> expected;
        for (int i = 0; i < k; ++i) expected.push_back(tables::probe_symbol(*spec, i, false));
        for (long long b = 0; b < blocks; ++b) {
            for (int f = 0; f < il->frames; ++f) {
                const long long g = b * il->frames + f;
                const bool boundary = g % long_il->frames == long_il->frames - 2;
                shifts += boundary ? 1 : 0;
                const bool last = b == blocks - 1 && f == il->frames - 1;
                const int length = k + (last ? static_cast<int>(eot) : 0);
                for (int i = 0; i < length; ++i) expected.push_back(tables::probe_symbol(*spec, i, boundary));
            }
        }
        REQUIRE(probes.size() == expected.size());
        for (std::size_t i = 0; i < probes.size(); ++i) {
            REQUIRE(probes[i].real() == expected[i].real());
            REQUIRE(probes[i].imag() == expected[i].imag());
        }

        // The Long interleaver's own boundary is the standard one: one shift per block.
        if (m.str("interleaver") == "long") CHECK(shifts == blocks);
        cases_with_shift += shifts > 0 ? 1 : 0;

        // The 255 markers in the burst are exactly those probe symbols.
        const auto burst = c.bytes("burst_symbols");
        std::size_t marked = 0;
        for (auto s : burst) marked += s == 255 ? 1 : 0;
        CHECK(marked == probes.size());
        ++checked;
    }
    CHECK(checked > 100);
    CHECK(cases_with_shift > 5);  // the short and medium cases that reach the Long interleaver's boundary
}

TEST_CASE("reference preamble starts with the TLC and Fixed sections from the tables", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    for (const ReferenceCase& c : load_cases(dir)) {
        const Manifest& m = c.manifest();
        INFO("case " << c.name());
        const auto chips = c.bytes("preamble_symbols");
        const std::size_t tlc_chips =
            static_cast<std::size_t>(m.integer("tlc_blocks")) * tables::preamble_chips_per_symbol;
        REQUIRE(chips.size() > tlc_chips + 288);

        // TLC: the complex conjugate of the fixedPN sequence; conjugate of 8PSK symbol s is
        // (8 - s) mod 8 (D.5.2.1.2).
        for (std::size_t i = 0; i < tlc_chips; ++i) {
            REQUIRE(chips[i] == ((8 - tables::fixed_pn[i % 256]) & 7));
        }

        // Fixed subsection of the first super-frame: 9 Walsh symbols, or one (di-bit 3) when
        // M = 1; the Walsh sequence repeated over 32 chips plus fixedPN modulo 8 (D.5.2.1.1).
        std::vector<std::uint8_t> dibits(tables::fixed_dibits.begin(), tables::fixed_dibits.end());
        if (m.integer("preamble_superframes") == 1) dibits = {3};
        std::size_t pos = 0;
        for (std::uint8_t d : dibits) {
            for (int i = 0; i < tables::preamble_chips_per_symbol; ++i, ++pos) {
                const unsigned expected = (tables::walsh[d][static_cast<std::size_t>(i) & 3] + tables::fixed_pn[pos % 256]) & 7;
                REQUIRE(chips[tlc_chips + pos] == expected);
            }
        }
    }
}
