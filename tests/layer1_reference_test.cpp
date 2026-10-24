// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer 1 against the C# reference files, stand-alone (docs/dev/layer1.md): for each of the
// 122 cases, every bit stage that exists in the C++ is recomputed from the PREVIOUS stage's
// reference file and compared bit for bit, naming the first stage that differs. Transcoding
// is checked in reference_tables_test.cpp and is not repeated here. Skipped when the files
// have not been generated.
//
// The stages run in pipeline order in first_differing_stage(): frame, preamble, encode,
// puncture, interleave, scramble, then burst (the whole symbol stream from the interleaved
// bits, with the probe values). A stage added to the pipeline is one more block there.
// framing_test.cpp holds further checks of the same files (block-at-a-time assembly, the
// waveform ID and downcount fields, values printed in the standard).

#include <m0lte/ms110d/mode.hpp>

#include "burst.hpp"
#include "fec/convolutional.hpp"
#include "framing.hpp"
#include "interleaver.hpp"
#include "preamble.hpp"
#include "puncture.hpp"
#include "reference_files.hpp"
#include "scrambler.hpp"
#include "trinomial_scrambler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace m0lte;
using namespace m0lte::ms110d;
using ms110d_test::Manifest;
using ms110d_test::ReferenceCase;

namespace {

using Bytes = std::vector<std::uint8_t>;

InterleaverKind kind_of(const std::string& s) {
    if (s == "ultrashort") return InterleaverKind::UltraShort;
    if (s == "short") return InterleaverKind::Short;
    if (s == "medium") return InterleaverKind::Medium;
    return InterleaverKind::Long;
}

// Empty string when equal, else a description of the first difference.
std::string diff(const Bytes& expected, const Bytes& actual) {
    std::ostringstream os;
    if (expected.size() != actual.size()) {
        os << "length: reference " << expected.size() << ", C++ " << actual.size();
        return os.str();
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            os << "index " << i << ": reference " << int(expected[i]) << ", C++ " << int(actual[i]);
            return os.str();
        }
    }
    return {};
}

// The reference files' form of the symbol stream: symbol bytes, 255 for a probe symbol.
struct RecordingSink {
    Bytes symbols;
    std::vector<std::complex<float>> probes;
    void psk(std::uint8_t s) { symbols.push_back(s); }
    void qam(std::uint8_t s) { symbols.push_back(s); }
    void probe(tables::Complex32 v) {
        symbols.push_back(255);
        probes.push_back(v);
    }
};

// "stage (detail)" for the first stage that differs, or "" when every stage matches.
std::string first_differing_stage(const ReferenceCase& c) {
    const Manifest& m = c.manifest();
    const int wn = static_cast<int>(m.integer("wn"));
    const auto mode = mode_3k(wn);
    const auto il = interleaver_3k(wn, kind_of(m.str("interleaver")));
    if (!mode || !il) return "tables (mode or interleaver missing)";
    const fec::ConvolutionalCode& code = m.integer("constraint_length") == 7 ? fec::k7_133_171 : fec::k9_561_753;
    const std::size_t blocks = static_cast<std::size_t>(m.integer("blocks"));
    const std::size_t in_bits = static_cast<std::size_t>(il->input_bits);
    const std::size_t size = static_cast<std::size_t>(il->size_bits);

    // frame: payload -> framed bits (EOM and zero fill).
    {
        const Bytes payload = c.bytes("payload_bits");
        Bytes out(blocks * in_bits);
        if (framed_block_count(payload.size(), m.integer("append_eom") != 0, in_bits) != blocks) return "frame (block count)";
        if (!build_tx_bits(payload, m.integer("append_eom") != 0, in_bits, out)) return "frame (rejected)";
        if (auto d = diff(c.bytes("framed_bits"), out); !d.empty()) return "frame (" + d + ")";
    }

    // preamble: the 8PSK chips.
    const auto preamble = Preamble::make(static_cast<int>(m.integer("tlc_blocks")),
                                         static_cast<int>(m.integer("preamble_superframes")), wn,
                                         kind_of(m.str("interleaver")), static_cast<int>(m.integer("constraint_length")));
    if (!preamble) return "preamble (rejected)";
    {
        Bytes out(preamble->chip_count());
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = preamble->chip(i);
        if (auto d = diff(c.bytes("preamble_symbols"), out); !d.empty()) return "preamble (" + d + ")";
    }

    // encode: framed bits -> encoded bits, block by block.
    {
        const Bytes framed = c.bytes("framed_bits");
        Bytes out(2 * framed.size());
        if (framed.size() != blocks * in_bits) return "encode (framed bits are not whole blocks)";
        for (std::size_t b = 0; b < blocks; ++b) {
            if (!fec::encode_tail_biting(code, std::span(framed).subspan(b * in_bits, in_bits),
                                         std::span(out).subspan(b * 2 * in_bits, 2 * in_bits))) {
                return "encode (rejected)";
            }
        }
        if (auto d = diff(c.bytes("encoded_bits"), out); !d.empty()) return "encode (" + d + ")";
    }

    // puncture: encoded bits -> punctured bits, block by block.
    {
        const Bytes encoded = c.bytes("encoded_bits");
        const PunctureSpec* spec = find_puncture(code, static_cast<unsigned>(mode->code_rate.numerator),
                                                 static_cast<unsigned>(mode->code_rate.denominator));
        if (spec == nullptr) return "puncture (no Table D-L entry)";
        Bytes out(blocks * size);
        for (std::size_t b = 0; b < blocks; ++b) {
            const auto w = apply_puncture(*spec, std::span(encoded).subspan(b * 2 * in_bits, 2 * in_bits),
                                          std::span(out).subspan(b * size, size));
            if (!w || *w != size) return "puncture (wrong length)";
        }
        if (auto d = diff(c.bytes("punctured_bits"), out); !d.empty()) return "puncture (" + d + ")";
    }

    // interleave: punctured bits -> interleaved bits, block by block.
    {
        const Bytes punctured = c.bytes("punctured_bits");
        const Interleaver ilv(static_cast<std::uint32_t>(il->size_bits), static_cast<std::uint32_t>(il->increment));
        Bytes out(punctured.size());
        for (std::size_t b = 0; b < blocks; ++b) ilv.interleave(punctured.data() + b * size, out.data() + b * size);
        if (auto d = diff(c.bytes("interleaved_bits"), out); !d.empty()) return "interleave (" + d + ")";
    }

    // scramble: the scramble sequence, then the data symbols from the transcoded file.
    {
        const Bytes transcoded = c.bytes("transcoded_symbols");
        const Bytes want_seq = c.bytes("scramble_symbols");
        Bytes seq(want_seq.size());
        Bytes data(transcoded.size());
        if (wn == 0) {
            // Trinomial scrambler, reset at every interleaver boundary (one block = frames Walsh symbols of 32 chips).
            const std::size_t block = static_cast<std::size_t>(il->frames) * 32;
            Trinomial159Scrambler t;
            for (std::size_t i = 0; i < seq.size(); ++i) {
                if (i % block == 0) t.reset();
                seq[i] = static_cast<std::uint8_t>(t.next());
                data[i] = static_cast<std::uint8_t>((transcoded[i] + seq[i]) & 7);
            }
        } else {
            const std::size_t u = static_cast<std::size_t>(mode->u);
            const bool qam = mode->modulation == Modulation::Qam16;
            Scrambler s;
            Scrambler s0;
            for (std::size_t i = 0; i < seq.size(); ++i) {
                if (i % u == 0) {
                    s.reset();
                    s0.reset();
                }
                seq[i] = static_cast<std::uint8_t>(qam ? s0.next_qam(0, mode->bits_per_symbol) : s0.next_psk(0));
                data[i] = static_cast<std::uint8_t>(qam ? s.next_qam(transcoded[i], mode->bits_per_symbol)
                                                        : s.next_psk(transcoded[i]));
            }
        }
        if (auto d = diff(want_seq, seq); !d.empty()) return "scramble sequence (" + d + ")";
        if (auto d = diff(c.bytes("data_symbols"), data); !d.empty()) return "scramble data (" + d + ")";
    }

    // burst: the whole symbol stream from the interleaved bits, and the mini-probe values.
    {
        TxSettings cfg;
        cfg.waveform_number = wn;
        cfg.interleaver = kind_of(m.str("interleaver"));
        cfg.constraint_length = static_cast<int>(m.integer("constraint_length"));
        cfg.tlc_blocks = static_cast<int>(m.integer("tlc_blocks"));
        cfg.preamble_superframes = static_cast<int>(m.integer("preamble_superframes"));
        cfg.append_eom = m.integer("append_eom") != 0;
        cfg.append_eot = m.integer("append_eot") != 0;
        const auto layout = burst_layout(cfg, static_cast<std::size_t>(m.integer("payload_bits")));
        if (!layout) return "burst (no layout)";
        RecordingSink sink;
        if (!assemble_burst(*layout, *preamble, c.bytes("interleaved_bits"), sink)) return "burst (rejected)";
        if (auto d = diff(c.bytes("burst_symbols"), sink.symbols); !d.empty()) return "burst symbols (" + d + ")";
        const auto probes = c.complex32("probe_values_cf32le");
        if (probes.size() != sink.probes.size()) return "burst probes (count)";
        for (std::size_t i = 0; i < probes.size(); ++i) {
            if (probes[i].real() != sink.probes[i].real() || probes[i].imag() != sink.probes[i].imag()) {
                return "burst probes (index " + std::to_string(i) + ")";
            }
        }
    }

    return {};
}

}  // namespace

TEST_CASE("reference layer 1: every bit stage recomputed from the previous stage's file", "[reference][layer1]") {
    const std::filesystem::path dir = ms110d_test::reference_dir_or_skip();
    const auto index = ms110d_test::load_index(dir);
    int checked = 0;
    for (const auto& name : index.cases) {
        const ReferenceCase c(dir / name, name);
        const std::string stage = first_differing_stage(c);
        INFO("case " << name << ": first stage that differs: " << stage);
        REQUIRE(stage.empty());
        ++checked;
    }
    CHECK(checked == 122);
}
