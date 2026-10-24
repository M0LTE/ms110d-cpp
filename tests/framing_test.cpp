// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Framing, preamble and whole-burst symbol stream against the C# reference files (every case
// in the standard set), plus checks of the waveform ID and downcount fields, and a few
// values printed in MIL-STD-188-110D itself. The FEC stage is not run here: each case's
// interleaved bits come from the reference files, so this stands apart from the encoder.

#include <m0lte/ms110d/mode.hpp>

#include "alloc_guard.hpp"
#include "burst.hpp"
#include "framing.hpp"
#include "modulator.hpp"
#include "preamble.hpp"
#include "reference_files.hpp"
#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
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
    return InterleaverKind::Long;
}

std::vector<ReferenceCase> load_cases(const fs::path& dir) {
    const auto index = ms110d_test::load_index(dir);
    std::vector<ReferenceCase> cases;
    for (const auto& name : index.cases) cases.emplace_back(dir / name, name);
    return cases;
}

TxSettings config_of(const Manifest& m) {
    TxSettings c;
    c.waveform_number = static_cast<int>(m.integer("wn"));
    c.interleaver = kind_from_name(m.str("interleaver"));
    c.constraint_length = static_cast<int>(m.integer("constraint_length"));
    c.tlc_blocks = static_cast<int>(m.integer("tlc_blocks"));
    c.preamble_superframes = static_cast<int>(m.integer("preamble_superframes"));
    c.append_eom = m.integer("append_eom") != 0;
    c.append_eot = m.integer("append_eot") != 0;
    return c;
}

// Collects the stream in the reference files' form: symbol bytes, 255 for a probe symbol,
// and the probe values apart.
struct RecordingSink {
    std::vector<std::uint8_t> symbols;
    std::vector<std::complex<float>> probes;
    void psk(std::uint8_t s) { symbols.push_back(s); }
    void qam(std::uint8_t s) { symbols.push_back(s); }
    void probe(tables::Complex32 v) {
        symbols.push_back(255);
        probes.push_back(v);
    }
};

}  // namespace

TEST_CASE("reference framed bits match the payload, EOM and zero fill", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const Manifest& m = c.manifest();
        const auto payload = c.bytes("payload_bits");
        const auto expected = c.bytes("framed_bits");
        const auto input_bits = static_cast<std::size_t>(m.integer("block_input_bits"));
        const bool eom = m.integer("append_eom") != 0;

        const std::size_t blocks = framed_block_count(payload.size(), eom, input_bits);
        REQUIRE(blocks == static_cast<std::size_t>(m.integer("blocks")));
        std::vector<std::uint8_t> out(blocks * input_bits);
        REQUIRE(build_tx_bits(payload, eom, input_bits, out));
        CHECK(out == expected);
        ++checked;
    }
    CHECK(checked == 122);
}

TEST_CASE("reference preamble symbols match the generator", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const TxSettings cfg = config_of(c.manifest());
        const auto preamble =
            Preamble::make(cfg.tlc_blocks, cfg.preamble_superframes, cfg.waveform_number, cfg.interleaver, cfg.constraint_length);
        REQUIRE(preamble.has_value());
        const auto expected = c.bytes("preamble_symbols");
        REQUIRE(preamble->chip_count() == expected.size());
        std::vector<std::uint8_t> out(expected.size());
        preamble->write(out);
        CHECK(out == expected);
        ++checked;
    }
    CHECK(checked == 122);
}

TEST_CASE("reference whole burst symbol streams and probe values match", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int checked = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const Manifest& m = c.manifest();
        const TxSettings cfg = config_of(m);
        const auto payload_bits = static_cast<std::size_t>(m.integer("payload_bits"));
        const auto layout = burst_layout(cfg, payload_bits);
        REQUIRE(layout.has_value());
        CHECK(layout->blocks == static_cast<std::size_t>(m.integer("blocks")));
        CHECK(layout->preamble_symbols == static_cast<std::size_t>(m.integer("preamble_symbol_count")));
        CHECK(layout->data_symbols == static_cast<std::size_t>(m.integer("data_symbol_count")));
        CHECK(layout->total_symbols == static_cast<std::size_t>(m.integer("burst_symbol_count")));

        const auto preamble =
            Preamble::make(cfg.tlc_blocks, cfg.preamble_superframes, cfg.waveform_number, cfg.interleaver, cfg.constraint_length);
        REQUIRE(preamble.has_value());
        const auto wire = c.bytes("interleaved_bits");
        REQUIRE(wire.size() == layout->wire_bits);

        RecordingSink sink;
        REQUIRE(assemble_burst(*layout, *preamble, wire, sink));
        const auto burst = c.bytes("burst_symbols");
        REQUIRE(sink.symbols.size() == burst.size());
        CHECK(sink.symbols == burst);

        const auto probes = c.complex32("probe_values_cf32le");
        REQUIRE(sink.probes.size() == probes.size());
        bool probes_equal = true;
        for (std::size_t i = 0; i < probes.size(); ++i) {
            probes_equal = probes_equal && probes[i].real() == sink.probes[i].real() &&
                           probes[i].imag() == sink.probes[i].imag();
        }
        CHECK(probes_equal);

        // The non-probe symbols after the preamble are the reference's scrambled data symbols.
        std::vector<std::uint8_t> data;
        for (std::size_t i = layout->preamble_symbols; i < sink.symbols.size(); ++i) {
            if (sink.symbols[i] != 255) data.push_back(sink.symbols[i]);
        }
        CHECK(data == c.bytes("data_symbols"));
        ++checked;
    }
    CHECK(checked == 122);
}

TEST_CASE("reference block-at-a-time assembly equals the whole burst", "[reference]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    int ran = 0;
    for (const ReferenceCase& c : load_cases(dir)) {
        const Manifest& m = c.manifest();
        if (m.integer("blocks") < 2) continue;
        ++ran;
        INFO("case " << c.name());
        const TxSettings cfg = config_of(m);
        const auto layout = burst_layout(cfg, static_cast<std::size_t>(m.integer("payload_bits")));
        const auto preamble =
            Preamble::make(cfg.tlc_blocks, cfg.preamble_superframes, cfg.waveform_number, cfg.interleaver, cfg.constraint_length);
        const auto wire = c.bytes("interleaved_bits");
        RecordingSink sink;
        write_head(*layout, *preamble, sink);
        const auto size = static_cast<std::size_t>(layout->interleaver.size_bits);
        for (std::size_t b = 0; b < layout->blocks; ++b) {
            REQUIRE(write_block(*layout, b, std::span<const std::uint8_t>(wire).subspan(b * size, size), sink));
        }
        CHECK(sink.symbols == c.bytes("burst_symbols"));
    }
    CHECK(ran >= 10);
}

TEST_CASE("framing edge cases", "[framing]") {
    std::vector<std::uint8_t> out;
    // Empty payload without EOM still sends one all-zero block.
    CHECK(framed_block_count(0, false, 96) == 1);
    out.assign(96, 7);
    REQUIRE(build_tx_bits({}, false, 96, out));
    for (auto b : out) CHECK(b == 0);
    // EOM ending exactly on the block end adds no block; one more bit adds one.
    CHECK(framed_block_count(64, true, 96) == 1);
    CHECK(framed_block_count(65, true, 96) == 2);
    CHECK(framed_block_count(10, true, 0) == 0);
    // Wrong output size or a zero block size is refused and writes nothing.
    out.assign(95, 9);
    CHECK_FALSE(build_tx_bits({}, false, 96, out));
    CHECK(out[0] == 9);
    CHECK_FALSE(build_tx_bits({}, false, 0, out));
    // The EOM goes out leftmost bit first: 0x4B = 0100 1011.
    out.assign(96, 0);
    REQUIRE(build_tx_bits({}, true, 96, out));
    const std::vector<std::uint8_t> head{0, 1, 0, 0, 1, 0, 1, 1};
    CHECK(std::vector<std::uint8_t>(out.begin(), out.begin() + 8) == head);
}

TEST_CASE("waveform ID di-bits follow D.5.2.1.3.2 and round trip", "[framing]") {
    int decoded = 0;
    for (int wn = 0; wn <= 13; ++wn) {
        for (int il = 0; il < 4; ++il) {
            for (int k : {7, 9}) {
                const auto kind = static_cast<InterleaverKind>(il);
                const auto w = encode_wid(wn, kind, k);
                REQUIRE(w.has_value());
                // Rebuild the ten bits d9..d0 from the di-bits and check the printed equations.
                unsigned d = 0;
                for (auto x : *w) d = (d << 2) | x;
                const auto bit = [&](int n) { return (d >> n) & 1u; };
                CHECK(((d >> 6) & 15u) == static_cast<unsigned>(wn));
                CHECK(((d >> 4) & 3u) == static_cast<unsigned>(il));
                CHECK(bit(3) == (k == 9 ? 1u : 0u));
                CHECK(bit(2) == (bit(9) ^ bit(8) ^ bit(7)));
                CHECK(bit(1) == (bit(7) ^ bit(6) ^ bit(5)));
                CHECK(bit(0) == (bit(5) ^ bit(4) ^ bit(3)));
                const auto back = decode_wid(*w);
                if (wn < 14) {
                    REQUIRE(back.has_value());
                    CHECK(*back == WaveformId{wn, kind, k});
                    ++decoded;
                }
            }
        }
    }
    CHECK(decoded == 14 * 4 * 2);
    CHECK_FALSE(encode_wid(14, InterleaverKind::Short, 7).has_value());
    CHECK_FALSE(encode_wid(-1, InterleaverKind::Short, 7).has_value());
    CHECK_FALSE(encode_wid(6, InterleaverKind::Short, 8).has_value());
}

TEST_CASE("waveform ID decode accepts exactly the valid di-bit sets", "[framing]") {
    // Check decode against its definition over every one of the 1024 di-bit combinations.
    for (unsigned v = 0; v < 1024; ++v) {
        const std::array<std::uint8_t, 5> dibits{static_cast<std::uint8_t>((v >> 8) & 3), static_cast<std::uint8_t>((v >> 6) & 3),
                                                 static_cast<std::uint8_t>((v >> 4) & 3), static_cast<std::uint8_t>((v >> 2) & 3),
                                                 static_cast<std::uint8_t>(v & 3)};
        const auto id = decode_wid(dibits);
        const int wn = static_cast<int>(v >> 6);
        const auto expected = encode_wid(wn, static_cast<InterleaverKind>((v >> 4) & 3), ((v >> 3) & 1) != 0 ? 9 : 7);
        const bool should_pass = wn < 14 && expected && *expected == dibits;
        CHECK(id.has_value() == should_pass);
    }
    CHECK_FALSE(decode_wid({4, 0, 0, 0, 0}).has_value());
}

TEST_CASE("downcount di-bits follow D.5.2.1.3.1 and round trip", "[framing]") {
    for (int count = 0; count < 32; ++count) {
        const auto c = encode_count(count);
        REQUIRE(c.has_value());
        unsigned b = 0;
        for (auto x : *c) b = (b << 2) | x;  // b7..b0
        const auto bit = [&](int n) { return (b >> n) & 1u; };
        CHECK((b & 31u) == static_cast<unsigned>(count));
        CHECK(bit(7) == (bit(1) ^ bit(2) ^ bit(3)));
        CHECK(bit(6) == (bit(2) ^ bit(3) ^ bit(4)));
        CHECK(bit(5) == (bit(0) ^ bit(1) ^ bit(2)));
        const auto back = decode_count(*c);
        REQUIRE(back.has_value());
        CHECK(*back == count);
    }
    CHECK_FALSE(encode_count(32).has_value());
    CHECK_FALSE(encode_count(-1).has_value());
    // Flipping the parity bit b7 is caught.
    auto c = *encode_count(5);
    c[0] ^= 2;
    CHECK_FALSE(decode_count(c).has_value());
}

TEST_CASE("downcount decode accepts exactly the valid di-bit sets", "[framing]") {
    // Every one of the 256 di-bit sets: accepted only if it is the encoding of its own count.
    int accepted = 0;
    for (unsigned v = 0; v < 256; ++v) {
        const std::array<std::uint8_t, 4> d{static_cast<std::uint8_t>((v >> 6) & 3), static_cast<std::uint8_t>((v >> 4) & 3),
                                            static_cast<std::uint8_t>((v >> 2) & 3), static_cast<std::uint8_t>(v & 3)};
        const int count = static_cast<int>(v & 31u);
        const bool valid = *encode_count(count) == d;
        const auto got = decode_count(d);
        CHECK(got.has_value() == valid);
        if (valid) CHECK(*got == count);
        accepted += got.has_value() ? 1 : 0;
    }
    CHECK(accepted == 32);
    CHECK_FALSE(decode_count({4, 0, 0, 0}).has_value());
}

TEST_CASE("preamble layout: count down to zero and the WID in every super-frame", "[framing]") {
    const auto p = Preamble::make(2, 4, 6, InterleaverKind::Short, 7);
    REQUIRE(p.has_value());
    CHECK(p->chip_count() == 32u * (2 + 4 * 18));
    CHECK(Preamble::chip_count(0, 1) == 32u * 10);
    CHECK_FALSE(Preamble::make(256, 3, 6, InterleaverKind::Short, 7).has_value());
    CHECK_FALSE(Preamble::make(0, 0, 6, InterleaverKind::Short, 7).has_value());
    CHECK_FALSE(Preamble::make(0, 33, 6, InterleaverKind::Short, 7).has_value());
    // Each super-frame's count section differs, the WID section does not.
    const std::size_t base = 32u * 2;
    const std::size_t sf = 32u * 18;
    for (std::size_t rep = 1; rep < 4; ++rep) {
        bool wid_same = true;
        bool count_same = true;
        for (std::size_t i = 0; i < 32u * 5; ++i) {
            wid_same = wid_same && p->chip(base + rep * sf + 32u * 13 + i) == p->chip(base + 32u * 13 + i);
        }
        for (std::size_t i = 0; i < 32u * 4; ++i) {
            count_same = count_same && p->chip(base + rep * sf + 32u * 9 + i) == p->chip(base + 32u * 9 + i);
        }
        CHECK(wid_same);
        CHECK_FALSE(count_same);
    }
}

// Values printed in the standard, independent of the C#.

TEST_CASE("standard: Table D-XXII(c) shifted 24 symbol mini-probe", "[standard]") {
    // D.5.2.2 and Table D-XXII(c): the shifted probe begins 6 symbols into the 13 symbol base
    // sequence: the last 7 symbols of the base, a whole base, then its first 4 symbols.
    const int expected[24] = {-1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1};
    const tables::ProbeSpec* spec = tables::find_probe_spec(24);
    REQUIRE(spec != nullptr);
    for (int i = 0; i < 24; ++i) {
        CHECK(tables::probe_symbol(*spec, i, true).real() == static_cast<float>(expected[i]));
        CHECK(tables::probe_symbol(*spec, i, true).imag() == 0.0f);
    }
}

TEST_CASE("standard: Table D-LXIII EOT extension of the 24 symbol mini-probe", "[standard]") {
    // Symbols 0 to 55: four whole 13 symbol base sequences plus the first 4 symbols of a fifth.
    const int base[13] = {1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1};
    struct Capture {
        std::vector<float> v;
        void psk(std::uint8_t) {}
        void qam(std::uint8_t) {}
        void probe(tables::Complex32 s) { v.push_back(s.real()); }
    } sink;
    write_probe(24, false, 32, sink);
    REQUIRE(sink.v.size() == 56);
    for (int i = 0; i < 56; ++i) CHECK(sink.v[static_cast<std::size_t>(i)] == static_cast<float>(base[i % 13]));
}

TEST_CASE("standard: WN 0 worked example of D.5.1.4", "[standard]") {
    // Di-bit 01 is Walsh 0 4 0 4 repeated, added modulo 8 to the first 32 scramble symbols,
    // giving the 32 chips printed in the standard. The scrambler restarts at the block start,
    // so they are the first 32 data chips of a block whose first two fetched bits are 0, 1.
    const std::uint8_t printed[32] = {5, 2, 2, 5, 7, 7, 1, 5, 6, 4, 5, 0, 0, 3, 7, 4,
                                      5, 7, 1, 7, 3, 6, 2, 1, 5, 0, 7, 7, 5, 0, 3, 4};
    TxSettings cfg;
    cfg.waveform_number = 0;
    cfg.interleaver = InterleaverKind::Short;
    const auto layout = burst_layout(cfg, 0);
    REQUIRE(layout.has_value());
    std::vector<std::uint8_t> wire(layout->wire_bits, 0);
    wire[1] = 1;
    RecordingSink sink;
    REQUIRE(write_block(*layout, 0, wire, sink));
    REQUIRE(sink.symbols.size() >= 32);
    for (std::size_t i = 0; i < 32; ++i) CHECK(sink.symbols[i] == printed[i]);
}

TEST_CASE("burst assembly does not allocate", "[framing]") {
    TxSettings cfg;
    cfg.waveform_number = 6;
    const auto layout = burst_layout(cfg, 100);
    REQUIRE(layout.has_value());
    const auto preamble = Preamble::make(0, 3, 6, InterleaverKind::Short, 7);
    REQUIRE(preamble.has_value());
    std::vector<std::uint8_t> wire(layout->wire_bits, 1);
    struct Count {
        std::size_t n = 0;
        void psk(std::uint8_t) { ++n; }
        void qam(std::uint8_t) { ++n; }
        void probe(tables::Complex32) { ++n; }
    } sink;
    bool ok = false;
    std::size_t seen = 0;
    {
        const ms110d_test::AllocationScope scope;
        ok = assemble_burst(*layout, *preamble, wire, sink);
        seen = scope.allocations();
    }
    CHECK(ok);
    CHECK(seen == 0);
    CHECK(sink.n == layout->total_symbols);
}

// The C# package's own output for every waveform ID and downcount (tests/data, made by
// tools/csharp-vectors in framing mode).
TEST_CASE("waveform ID and downcount di-bits match the C# package for every input", "[framing]") {
    std::ifstream in(MS110D_CSHARP_FRAMING_DUMP);
    REQUIRE(in.good());
    std::string line;
    int wids = 0;
    int counts = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "wid") {
            int wn, il, k, d[5];
            ss >> wn >> il >> k >> d[0] >> d[1] >> d[2] >> d[3] >> d[4];
            const auto w = encode_wid(wn, static_cast<InterleaverKind>(il), k);
            REQUIRE(w.has_value());
            for (std::size_t i = 0; i < 5; ++i) CHECK(static_cast<int>((*w)[i]) == d[i]);
            ++wids;
        } else if (kind == "count") {
            int count, d[4];
            ss >> count >> d[0] >> d[1] >> d[2] >> d[3];
            const auto c = encode_count(count);
            REQUIRE(c.has_value());
            for (std::size_t i = 0; i < 4; ++i) CHECK(static_cast<int>((*c)[i]) == d[i]);
            ++counts;
        }
    }
    CHECK(wids == 14 * 4 * 2);
    CHECK(counts == 32);
}

namespace {

// Feeds each burst symbol straight into the modulator and collects the audio as it appears.
struct ModulatorSink {
    Modulator& mod;
    std::vector<real>& audio;
    void push(const tables::Complex32& p) {
        REQUIRE(mod.push_symbol(p));
        const std::size_t at = audio.size();
        audio.resize(at + mod.pending());
        mod.read(std::span<real>(audio).subspan(at));
    }
    void psk(std::uint8_t s) { push(tables::psk8[s]); }
    void qam(std::uint8_t s) { push(tables::qam16[s]); }
    void probe(tables::Complex32 v) { push(v); }
};

}  // namespace

TEST_CASE("reference end to end: framing into the modulator reproduces the reference audio", "[reference][modulator]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    constexpr double tolerance = 1e-6;  // the modulator's documented per-sample tolerance
    std::size_t cases = 0;
    double worst = 0;
    Modulator mod;
    for (const ReferenceCase& c : load_cases(dir)) {
        INFO("case " << c.name());
        const Manifest& m = c.manifest();
        TxSettings cfg = config_of(m);
        cfg.amplitude = static_cast<float>(m.real("amplitude"));
        cfg.roll_off = m.real("roll_off");
        REQUIRE(mod.configure(cfg));
        const auto layout = burst_layout(cfg, static_cast<std::size_t>(m.integer("payload_bits")));
        const auto preamble = Preamble::make(cfg.tlc_blocks, cfg.preamble_superframes, cfg.waveform_number,
                                             cfg.interleaver, cfg.constraint_length);
        REQUIRE((layout.has_value() && preamble.has_value()));

        // Only the interleaved bits come from the reference files (the FEC stage is not run here).
        const auto wire = c.bytes("interleaved_bits");
        std::vector<real> audio;
        audio.reserve(audio_length(layout->total_symbols, mod.taps()));
        mod.begin();
        ModulatorSink sink{mod, audio};
        REQUIRE(assemble_burst(*layout, *preamble, wire, sink));
        mod.end_of_symbols();
        const std::size_t at = audio.size();
        audio.resize(audio_length(layout->total_symbols, mod.taps()));
        mod.read(std::span<real>(audio).subspan(at));
        CHECK(mod.done());

        const auto ref = c.real32("audio_f32le");
        REQUIRE(audio.size() == ref.size());
        double err = 0;
        for (std::size_t i = 0; i < ref.size(); ++i) {
            err = std::max(err, std::fabs(static_cast<double>(audio[i]) - static_cast<double>(ref[i])));
        }
        worst = std::max(worst, err);
        CHECK(err <= tolerance);
        ++cases;
    }
    CHECK(cases == 122);
    INFO("worst error " << worst);
}

TEST_CASE("build_tx_block is the matching slice of build_tx_bits", "[framing]") {
    std::mt19937 rng(5);
    for (const std::size_t input : {24u, 40u, 96u, 288u}) {
        for (const bool eom : {true, false}) {
            for (const std::size_t n : {0u, 1u, 7u, 23u, 24u, 25u, 56u, 100u, 700u}) {
                std::vector<std::uint8_t> payload(n);
                for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 0xFFu);  // junk above the low bit
                const std::size_t blocks = framed_block_count(n, eom, input);
                std::vector<std::uint8_t> whole(blocks * input);
                REQUIRE(build_tx_bits(payload, eom, input, whole));
                for (std::size_t b = 0; b < blocks; ++b) {
                    std::vector<std::uint8_t> one(input, 9);
                    REQUIRE(build_tx_block(payload, eom, input, b, one));
                    CHECK(std::equal(one.begin(), one.end(), whole.begin() + static_cast<std::ptrdiff_t>(b * input)));
                }
                std::vector<std::uint8_t> out(input, 9);
                CHECK_FALSE(build_tx_block(payload, eom, input, blocks, out));
                CHECK_FALSE(build_tx_block(payload, eom, input, 0, std::span<std::uint8_t>(out).first(input - 1)));
                CHECK_FALSE(build_tx_block(payload, eom, 0, 0, std::span<std::uint8_t>{}));
                CHECK(out[0] == 9);
            }
        }
    }
}
