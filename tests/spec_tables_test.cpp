// Checks every C++ table against the transcriptions of MIL-STD-188-110D Appendix D in
// reference/spec-tables/ (transcribed twice from the standard; see the README there). This is
// the independent check against the standard: the C# values alone would only prove the port
// copied the C# faithfully. Each test names the table and the transcription file it uses.

#include <m0lte/ms110d/mode.hpp>

#include "spec_tables.hpp"
#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
using ms110d_test::CsvTable;
using ms110d_test::parse_float;
using ms110d_test::read_spec_csv;

namespace {

// The 3 kHz waveform numbers the library implements, as the transcriptions index them.
constexpr int k_wns[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 13};

int to_int(const std::string& s) {
    return std::stoi(s);
}

// "0110" -> 6; spaces are ignored ("0 1" -> 1).
int binary(const std::string& s) {
    int v = 0;
    for (char c : s) {
        if (c == '0' || c == '1') v = (v << 1) | (c - '0');
    }
    return v;
}

// The row of a bandwidth-indexed table (D-I, D-II, D-XI, D-XII, D-XIII, D-XLIX) for 3 kHz.
const std::vector<std::string>& row_3khz(const CsvTable& t) {
    for (const auto& r : t.rows) {
        if (!r.empty() && r[0] == "3") return r;
    }
    FAIL("no 3 kHz row");
    static const std::vector<std::string> none;
    return none;
}

// Column of waveform number wn in D-II / D-XLIX, whose headers are "<wn> (<modulation>)".
std::size_t wn_column_paren(const CsvTable& t, int wn) {
    const std::string prefix = std::to_string(wn) + " (";
    for (std::size_t i = 1; i < t.header.size(); ++i) {
        if (t.header[i].rfind(prefix, 0) == 0) return i;
    }
    FAIL("no column for WN " << wn);
    return 0;
}

// Column of waveform number wn in D-XI / D-XII, whose headers are "WN<wn> <modulation>".
std::size_t wn_column_wn(const CsvTable& t, int wn) {
    const std::string prefix = "WN" + std::to_string(wn) + " ";
    for (std::size_t i = 1; i < t.header.size(); ++i) {
        if (t.header[i].rfind(prefix, 0) == 0) return i;
    }
    FAIL("no column for WN " << wn);
    return 0;
}

Modulation modulation_from_name(const std::string& name) {
    if (name == "Walsh") return Modulation::Walsh;
    if (name == "BPSK") return Modulation::Bpsk;
    if (name == "QPSK") return Modulation::Qpsk;
    if (name == "8PSK") return Modulation::Psk8;
    if (name == "16QAM") return Modulation::Qam16;
    FAIL("unknown modulation " << name);
    return Modulation::Walsh;
}

CodeRate parse_rate(const std::string& s) {
    const auto slash = s.find('/');
    return {to_int(s.substr(0, slash)), to_int(s.substr(slash + 1))};
}

template <std::size_t N>
void check_pn(const std::array<std::uint8_t, N>& table, const std::string& file) {
    const auto spec = ms110d_test::read_spec_brace_ints(file);
    REQUIRE(spec.size() == N);
    for (std::size_t i = 0; i < N; ++i) {
        INFO(file << " entry " << i);
        CHECK(static_cast<int>(table[i]) == spec[i]);
    }
}

// A base-sequence CSV: symbol number, in-phase, quadrature. The printed values are compared
// exactly (as the float the compiler makes of the same decimal literal).
void check_base(std::span<const tables::Complex32> table, const std::string& file) {
    const CsvTable t = read_spec_csv(file);
    REQUIRE(t.rows.size() == table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
        INFO(file << " symbol " << i);
        REQUIRE(to_int(t.rows[i][0]) == static_cast<int>(i));
        CHECK(table[i].real() == parse_float(t.rows[i][1]));
        CHECK(table[i].imag() == parse_float(t.rows[i][2]));
    }
}

}  // namespace

TEST_CASE("constants match Tables D-I and D-XIII at 3 kHz", "[spec]") {
    const CsvTable t1 = read_spec_csv("d01-symbol-rates.csv");
    const auto& d1 = row_3khz(t1);
    CHECK(tables::symbol_rate == to_int(d1[1]));
    CHECK(tables::sub_carrier_hz == to_int(d1[2]));
    const CsvTable t13 = read_spec_csv("d13-preamble-channel-symbol-length.csv");
    const auto& d13 = row_3khz(t13);
    CHECK(tables::preamble_chips_per_symbol == to_int(d13[1]));
}

TEST_CASE("PN sequences match Tables D-XVIII, D-XIX and D-XX", "[spec]") {
    check_pn(tables::fixed_pn, "d18-fixedpn.txt");
    check_pn(tables::cnt_pn, "d19-cntpn.txt");
    check_pn(tables::wid_pn, "d20-widpn.txt");
}

TEST_CASE("Walsh sequences match Table D-XIV", "[spec]") {
    const CsvTable t = read_spec_csv("d14-walsh-sync-sequences.csv");
    REQUIRE(t.rows.size() == 4);
    for (const auto& r : t.rows) {
        const int dibit = binary(r[0]);
        REQUIRE(r[1].size() == 4);
        for (std::size_t i = 0; i < 4; ++i) {
            INFO("di-bit " << r[0] << " element " << i);
            CHECK(static_cast<int>(tables::walsh[static_cast<std::size_t>(dibit)][i]) == r[1][i] - '0');
        }
    }
}

TEST_CASE("Fixed-subsection di-bits match D.5.2.1.3", "[spec]") {
    const std::string text = ms110d_test::read_spec_text("preamble-fixed-tlc-prose.md");
    const auto anchor = text.find("For the case of 9 Walsh symbols");
    REQUIRE(anchor != std::string::npos);
    const auto open = text.find('{', anchor);
    const auto close = text.find('}', open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    std::vector<int> spec;
    for (std::size_t i = open + 1; i < close; ++i) {
        if (text[i] >= '0' && text[i] <= '9') spec.push_back(text[i] - '0');
    }
    REQUIRE(spec.size() == tables::fixed_dibits.size());
    for (std::size_t i = 0; i < spec.size(); ++i) CHECK(static_cast<int>(tables::fixed_dibits[i]) == spec[i]);
}

TEST_CASE("transcoding matches Tables D-III, D-IV and D-V", "[spec]") {
    const CsvTable bpsk = read_spec_csv("d03-transcoding-bpsk.csv");
    REQUIRE(bpsk.rows.size() == tables::transcode_bpsk.size());
    for (const auto& r : bpsk.rows) {
        CHECK(static_cast<int>(tables::transcode_bpsk[static_cast<std::size_t>(binary(r[0]))]) == to_int(r[1]));
    }
    const CsvTable qpsk = read_spec_csv("d04-transcoding-qpsk.csv");
    REQUIRE(qpsk.rows.size() == tables::transcode_qpsk.size());
    for (const auto& r : qpsk.rows) {
        CHECK(static_cast<int>(tables::transcode_qpsk[static_cast<std::size_t>(binary(r[0]))]) == to_int(r[1]));
    }
    const CsvTable psk8 = read_spec_csv("d05-transcoding-8psk.csv");
    REQUIRE(psk8.rows.size() == tables::transcode_8psk.size());
    for (const auto& r : psk8.rows) {
        CHECK(static_cast<int>(tables::transcode_8psk[static_cast<std::size_t>(binary(r[0]))]) == to_int(r[1]));
    }
}

TEST_CASE("8PSK constellation matches Table D-VI", "[spec]") {
    const CsvTable t = read_spec_csv("d06-8psk-symbol-mapping.csv");
    REQUIRE(t.rows.size() == tables::psk8.size());
    for (const auto& r : t.rows) {
        const auto s = static_cast<std::size_t>(to_int(r[0]));
        INFO("symbol " << s);
        CHECK(tables::psk8[s].real() == parse_float(r[2]));
        CHECK(tables::psk8[s].imag() == parse_float(r[3]));
    }
}

TEST_CASE("16-QAM constellation matches Table D-VII", "[spec]") {
    const CsvTable t = read_spec_csv("d07-constellation-16qam.csv");
    REQUIRE(t.rows.size() == tables::qam16.size());
    for (const auto& r : t.rows) {
        const auto s = static_cast<std::size_t>(to_int(r[0]));
        INFO("symbol " << s);
        CHECK(tables::qam16[s].real() == parse_float(r[1]));
        CHECK(tables::qam16[s].imag() == parse_float(r[2]));
    }
}

TEST_CASE("mini-probe base sequences match Tables D-XXII(a), D-XXIII, D-XXIV and D-XXV", "[spec]") {
    check_base(tables::base13, "d22a-base13-miniprobe.csv");
    check_base(tables::base16, "d23-base16-miniprobe.csv");
    check_base(tables::base19, "d24-base19-miniprobe.csv");
    check_base(tables::base25, "d25-base25-miniprobe.csv");
}

TEST_CASE("Base25 values are the fifth roots of unity to within printing error", "[spec][base25][issue-16]") {
    // The standard prints six decimals. Every entry is a fifth root of unity (0, 72, 144, 216
    // or 288 degrees), so the exact phases are known; four printed digits are one unit in the
    // last place away from correctly rounded (Table D-XXV rows 9, 12, 21 and 24). The C# sends
    // the printed digits and so does this library, for on-air compatibility; this pins that
    // the difference is only printing error.
    const double pi = std::acos(-1.0);
    const double tolerance = 2e-6;
    std::vector<std::size_t> not_correctly_rounded;
    for (std::size_t i = 0; i < tables::base25.size(); ++i) {
        const double re = static_cast<double>(tables::base25[i].real());
        const double im = static_cast<double>(tables::base25[i].imag());
        double best = 1e9;
        double best_re = 0;
        double best_im = 0;
        for (int m = 0; m < 5; ++m) {
            const double er = std::cos(2 * pi * m / 5);
            const double ei = std::sin(2 * pi * m / 5);
            const double d = std::hypot(re - er, im - ei);
            if (d < best) {
                best = d;
                best_re = er;
                best_im = ei;
            }
        }
        INFO("Base25 symbol " << i);
        CHECK(best <= tolerance);
        // Correctly rounded to six decimals means within half a unit of the last place.
        if (std::fabs(re - best_re) > 5.0e-7 + 1e-9 || std::fabs(im - best_im) > 5.0e-7 + 1e-9) {
            not_correctly_rounded.push_back(i);
        }
    }
    CHECK(not_correctly_rounded == std::vector<std::size_t>{9, 12, 21, 24});
}

TEST_CASE("mini-probe lengths, bases and shifts match Table D-XXI", "[spec]") {
    const CsvTable t = read_spec_csv("d21-miniprobes.csv");
    for (const auto& spec : tables::probe_specs) {
        const std::vector<std::string>* row = nullptr;
        for (const auto& r : t.rows) {
            if (to_int(r[0]) == spec.k) row = &r;
        }
        INFO("probe length " << spec.k);
        REQUIRE(row != nullptr);
        CHECK(static_cast<int>(spec.base.size()) == to_int((*row)[1]));
        CHECK(spec.shift == to_int((*row)[2]));
        CHECK(tables::find_probe_spec(spec.k) == &spec);
    }
    CHECK(tables::find_probe_spec(64) == nullptr);
    CHECK(tables::find_probe_spec(0) == nullptr);
    // The lengths the implemented waveforms use must all be present.
    for (const Mode& m : modes_3k()) {
        if (m.k != 0) CHECK(tables::find_probe_spec(m.k) != nullptr);
    }
}

TEST_CASE("mode rows match Tables D-II, D-XI, D-XII and D-XLIX", "[spec]") {
    const CsvTable d2 = read_spec_csv("d02-rate-modulation.csv");
    const CsvTable d11 = read_spec_csv("d11-unknown-data-symbols.csv");
    const CsvTable d12 = read_spec_csv("d12-known-miniprobe-symbols.csv");
    const CsvTable d49 = read_spec_csv("d49-code-rates.csv");
    const auto& r2 = row_3khz(d2);
    const auto& r11 = row_3khz(d11);
    const auto& r12 = row_3khz(d12);
    const auto& r49 = row_3khz(d49);

    for (int wn : k_wns) {
        INFO("WN " << wn);
        const auto mode = mode_3k(wn);
        REQUIRE(mode.has_value());
        CHECK(mode->wn == wn);

        const std::size_t c2 = wn_column_paren(d2, wn);
        const std::string& h2 = d2.header[c2];
        const std::string name = h2.substr(h2.find('(') + 1, h2.find(')') - h2.find('(') - 1);
        CHECK(mode->modulation == modulation_from_name(name));
        CHECK(mode->bps == to_int(r2[c2]));

        const std::string& u = r11[wn_column_wn(d11, wn)];
        const std::string& k = r12[wn_column_wn(d12, wn)];
        if (wn == 0) {
            CHECK(u == "N/A");
            CHECK(k == "N/A");
            CHECK(mode->u == 0);
            CHECK(mode->k == 0);
        } else {
            CHECK(mode->u == to_int(u));
            CHECK(mode->k == to_int(k));
        }

        CHECK(mode->code_rate == parse_rate(r49[wn_column_paren(d49, wn)]));
    }
}

TEST_CASE("the implemented waveform set is exactly WN 0 to 8 and 13", "[spec]") {
    const auto modes = modes_3k();
    REQUIRE(modes.size() == std::size(k_wns));
    for (std::size_t i = 0; i < modes.size(); ++i) CHECK(modes[i].wn == k_wns[i]);
    for (int wn : {-1, 9, 10, 11, 12, 14, 15, 100}) {
        INFO("WN " << wn);
        CHECK_FALSE(mode_3k(wn).has_value());
        CHECK_FALSE(has_interleaver_3k(wn, InterleaverKind::Short));
        CHECK_FALSE(interleaver_3k(wn, InterleaverKind::Short).has_value());
    }
}

TEST_CASE("interleaver sizes and increments match Tables D-XXXVII and D-LI", "[spec]") {
    const CsvTable d37 = read_spec_csv("d37-interleaver-3khz.csv");
    const CsvTable d51 = read_spec_csv("d51-interleaver-increments-3khz.csv");
    const InterleaverKind kinds[] = {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium,
                                     InterleaverKind::Long};

    int checked = 0;
    for (int wn : k_wns) {
        const std::vector<std::string>* r37 = nullptr;
        const std::vector<std::string>* r51 = nullptr;
        for (const auto& r : d37.rows) {
            if (to_int(r[0]) == wn) r37 = &r;
        }
        for (const auto& r : d51.rows) {
            if (to_int(r[0]) == wn) r51 = &r;
        }
        REQUIRE(r37 != nullptr);
        REQUIRE(r51 != nullptr);
        for (std::size_t i = 0; i < 4; ++i) {
            INFO("WN " << wn << " interleaver " << i);
            const std::string& frames = (*r37)[1 + 3 * i];
            const std::string& bits = (*r37)[2 + 3 * i];
            const std::string& input = (*r37)[3 + 3 * i];
            const std::string& inc = (*r51)[1 + i];
            const auto p = interleaver_3k(wn, kinds[i]);
            if (frames == "-") {
                // Table D-XXXVII dashes (WN 0 has no UltraShort); D-LI must agree.
                CHECK(bits == "-");
                CHECK(input == "-");
                CHECK(inc == "-");
                CHECK_FALSE(p.has_value());
                CHECK_FALSE(has_interleaver_3k(wn, kinds[i]));
            } else {
                REQUIRE(p.has_value());
                CHECK(has_interleaver_3k(wn, kinds[i]));
                CHECK(p->frames == to_int(frames));
                CHECK(p->size_bits == to_int(bits));
                CHECK(p->input_bits == to_int(input));
                CHECK(p->increment == to_int(inc));
                ++checked;
            }
        }
    }
    CHECK(checked == 39);
}

TEST_CASE("WID field mapping matches Tables D-XV, D-XVI and D-XVII", "[spec]") {
    const CsvTable d15 = read_spec_csv("d15-waveform-number-mapping.csv");
    int numbered = 0;
    for (const auto& r : d15.rows) {
        if (r[0] == "(reserved)") continue;
        const int wn = to_int(r[0]);
        INFO("WN " << wn);
        // w4 = d9 d8 and w3 = d7 d6, so the four bits d9d8d7d6 are the waveform number.
        CHECK(tables::wid_waveform_bits(wn) == ((binary(r[1]) << 2) | binary(r[2])));
        ++numbered;
    }
    CHECK(numbered == 14);

    const CsvTable d16 = read_spec_csv("d16-interleaver-selection-mapping.csv");
    REQUIRE(d16.rows.size() == 4);
    for (const auto& r : d16.rows) {
        InterleaverKind kind{};
        if (r[0] == "Ultra Short") kind = InterleaverKind::UltraShort;
        else if (r[0] == "Short") kind = InterleaverKind::Short;
        else if (r[0] == "Medium") kind = InterleaverKind::Medium;
        else if (r[0] == "Long") kind = InterleaverKind::Long;
        else FAIL("unknown interleaver " << r[0]);
        INFO(r[0]);
        CHECK(tables::wid_interleaver_bits(kind) == binary(r[1]));
    }

    const CsvTable d17 = read_spec_csv("d17-constraint-length-voice-mode-mapping.csv");
    REQUIRE(d17.rows.size() == 2);
    for (const auto& r : d17.rows) {
        CHECK(tables::wid_constraint_bit(to_int(r[0])) == to_int(r[1]));
    }
}

// The next tests check internal consistency and the few values with no transcription
// counterpart (the EOM word and the coded bits per symbol), so they rest on the C# and on the
// relations the standard's tables must satisfy.

TEST_CASE("EOM word is the C# value", "[csharp]") {
    // D.5.4.3 gives the EOM in prose, not as a transcribed table. The value is the C#
    // Ms110dTables.Eom; the reference files carry it as bits (checked in the reference test).
    CHECK(tables::eom == 0x4B65A5B2u);
}

TEST_CASE("coded bits per symbol follow the modulation", "[csharp]") {
    for (const Mode& m : modes_3k()) {
        INFO("WN " << m.wn);
        switch (m.modulation) {
            case Modulation::Walsh: CHECK(m.bits_per_symbol == 2); break;
            case Modulation::Bpsk: CHECK(m.bits_per_symbol == 1); break;
            case Modulation::Qpsk: CHECK(m.bits_per_symbol == 2); break;
            case Modulation::Psk8: CHECK(m.bits_per_symbol == 3); break;
            case Modulation::Qam16: CHECK(m.bits_per_symbol == 4); break;
        }
    }
}

TEST_CASE("mode and interleaver tables are consistent with each other", "[spec]") {
    const InterleaverKind kinds[] = {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium,
                                     InterleaverKind::Long};
    for (const Mode& m : modes_3k()) {
        for (InterleaverKind kind : kinds) {
            const auto p = interleaver_3k(m.wn, kind);
            if (!p) continue;
            INFO("WN " << m.wn << " interleaver " << static_cast<int>(kind));
            // The coded and punctured block fits the interleaver exactly (D.5.3.2).
            CHECK(p->input_bits * m.code_rate.denominator == p->size_bits * m.code_rate.numerator);
            // Frames of U symbols at bits_per_symbol fill the interleaver (WN 0: frames are the
            // Walsh channel symbols, two bits each).
            const int per_frame = m.wn == 0 ? 1 : m.u;
            CHECK(p->frames * per_frame * m.bits_per_symbol == p->size_bits);
        }
        if (m.wn != 0) {
            // User rate = symbol rate x U/(U+K) x bits per symbol x code rate.
            const long long num = 2400LL * m.u * m.bits_per_symbol * m.code_rate.numerator;
            const long long den = static_cast<long long>(m.u + m.k) * m.code_rate.denominator;
            CHECK(num == m.bps * den);
        }
    }
}
