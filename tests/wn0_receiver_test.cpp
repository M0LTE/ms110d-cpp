// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The WN 0 (Walsh) receiver against the C# demodulator: the same audio and the same burst
// start in, the same Walsh decisions and soft values out. Clean reference audio (all ten WN 0
// cases), seeded AWGN and Poor channel bursts (decision error rates), and the C++ transmitter
// into the C++ receiver. The C# numbers are in tests/data/csharp-wn0*-0.3.0.txt, made by
// tools/csharp-vectors (wn0, wn0-awgn and wn0-poor modes) from the real 0.3.0 demodulator.

#include <m0lte/ms110d/mode.hpp>
#include <m0lte/ms110d/transmitter.hpp>

#include "alloc_guard.hpp"
#include "fec/convolutional.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "framing.hpp"
#include "puncture.hpp"
#include "reference_files.hpp"
#include "shared_noise.hpp"
#include "wn0_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if !defined(MS110D_CSHARP_WN0_DUMP) || !defined(MS110D_CSHARP_WN0_AWGN) || !defined(MS110D_WN0_POOR_STEM)
#error "the build must define the WN 0 dump paths"
#endif

using namespace m0lte::ms110d;
namespace fec = m0lte::fec;
using ms110d_test::ReferenceCase;

namespace {

namespace fs = std::filesystem;

using Tokens = std::vector<std::string>;

std::vector<Tokens> read_dump(const char* path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::vector<Tokens> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Tokens t;
        std::string tok;
        while (ss >> tok) t.push_back(tok);
        out.push_back(t);
    }
    return out;
}

double num(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

struct Start {
    double chip0 = 0, tau = 0, omega = 0, theta = 0;
    std::int64_t data_start = 0;
    std::uint32_t il_bits = 0;
};

// "start chip0 X tau X omega X theta X data_start N il_bits N"
Start parse_start(const Tokens& t) {
    REQUIRE(t.size() == 13);
    REQUIRE(t[0] == "start");
    Start s;
    s.chip0 = num(t[2]);
    s.tau = num(t[4]);
    s.omega = num(t[6]);
    s.theta = num(t[8]);
    s.data_start = std::atoll(t[10].c_str());
    s.il_bits = static_cast<std::uint32_t>(std::atoi(t[12].c_str()));
    return s;
}

Wn0Start to_start(const Start& s, const Interleaver& il) {
    Wn0Start w;
    w.chip0 = s.chip0;
    w.tau = s.tau;
    w.omega = s.omega;
    w.theta_base = s.theta;
    w.data_start_chip = s.data_start;
    w.interleaver = il;
    return w;
}

Interleaver wn0_interleaver(InterleaverKind k) {
    const auto p = interleaver_3k(0, k);
    REQUIRE(p.has_value());
    return Interleaver(static_cast<std::uint32_t>(p->size_bits), static_cast<std::uint32_t>(p->increment));
}

InterleaverKind kind_from_name(const std::string& s) {
    if (s == "short") return InterleaverKind::Short;
    if (s == "medium") return InterleaverKind::Medium;
    return InterleaverKind::Long;
}

struct Result {
    std::vector<int> dibits;
    std::vector<double> llr;  // two per symbol, wire order, blocks one after the other
    std::vector<std::vector<double>> gains;
    std::vector<std::vector<double>> deint;  // per block, code order
    std::vector<std::string> info;           // per block, the decoded info bits as 0/1 text
    int blocks = 0;
    Wn0Step end = Wn0Step::Inactive;
};

constexpr int all_blocks = 1 << 30;

// Pushes the audio (then `tail_zeros` samples of silence) in chunks of rotating size, stepping
// after each, until `want_blocks` blocks are complete, the signal is lost or the audio is used up.
// Each block is also decoded with the Viterbi decoder.
Result run_rx(StandaloneWn0Receiver& rx, fec::TailBitingViterbiDecoder& dec, const std::vector<float>& audio, const Start& s,
              const Interleaver& il, int want_blocks, std::size_t gains_symbols = 0, std::size_t tail_zeros = 9600) {
    Result r;
    std::vector<real> block(il.size_bits());
    std::vector<real> deint(il.size_bits());
    std::vector<std::uint8_t> info(il.size_bits() / 2);
    REQUIRE(rx.start(to_start(s, il), block));
    static const std::size_t chunks[] = {2400, 7, 480, 1, 1200, 100};
    std::size_t at = 0, ci = 0;
    std::vector<float> src = audio;
    src.insert(src.end(), tail_zeros, 0.0f);
    bool done = false;
    while (!done && at < src.size()) {
        const std::size_t n = std::min(chunks[ci++ % std::size(chunks)], src.size() - at);
        rx.push_audio(std::span<const float>(src).subspan(at, n));
        at += n;
        for (;;) {
            const Wn0Step st = rx.step();
            if (st == Wn0Step::Symbol || st == Wn0Step::BlockComplete || st == Wn0Step::SignalLost) {
                r.dibits.push_back(rx.last().dibit);
                r.llr.push_back(static_cast<double>(rx.last().llr[0]));
                r.llr.push_back(static_cast<double>(rx.last().llr[1]));
                if (r.dibits.size() <= gains_symbols) {
                    std::vector<double> g;
                    for (int k = 0; k < Wn0Receiver::fingers; ++k) g.push_back(static_cast<double>(rx.gain_magnitude(k)));
                    r.gains.push_back(g);
                }
                if (st == Wn0Step::SignalLost) {
                    r.end = st;
                    done = true;
                    break;
                }
                if (st == Wn0Step::BlockComplete) {
                    REQUIRE(rx.deinterleaved_block(deint));
                    r.deint.emplace_back(deint.begin(), deint.end());
                    REQUIRE(rx.decode_block(dec, info));
                    std::string text;
                    for (const std::uint8_t bit : info) text.push_back(static_cast<char>('0' + bit));
                    r.info.push_back(text);
                    if (++r.blocks == want_blocks) {
                        done = true;
                        break;
                    }
                }
            } else {
                r.end = st;
                done = st == Wn0Step::SignalLost;
                break;
            }
        }
    }
    return r;
}

using ms110d_test::SharedNoise;

int truth_dibit(const std::vector<std::uint8_t>& wire, std::size_t sym) {
    return (wire[2 * sym] << 1) | wire[(2 * sym) + 1];
}

struct Rates {
    long symbols = 0, dibit_errors = 0, bits = 0, bit_errors = 0, disagree = 0, trials = 0;
    double der() const { return symbols ? static_cast<double>(dibit_errors) / static_cast<double>(symbols) : 0; }
    double ber() const { return bits ? static_cast<double>(bit_errors) / static_cast<double>(bits) : 0; }
};

void configured_rx(StandaloneWn0Receiver& rx) { REQUIRE(rx.configure(0.35)); }

// Poor channel files made by scripts/generate-wn0-poor.sh. Missing: skip, or fail in CI.
std::string poor_stem_or_skip() {
    const std::string stem = MS110D_WN0_POOR_STEM;
    if (std::ifstream(stem + ".txt").good() && std::ifstream(stem + ".i16le").good()) return stem;
    if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("the Poor channel files are missing: " << stem);
    SKIP("no Poor channel files at " << stem << " (run scripts/generate-wn0-poor.sh)");
    return {};
}

}  // namespace

TEST_CASE("reference wn0 receiver matches the C# on every WN 0 case's audio", "[reference][wn0]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const auto lines = read_dump(MS110D_CSHARP_WN0_DUMP);
    auto rx = std::make_unique<StandaloneWn0Receiver>();
    configured_rx(*rx);
    int decoded_blocks = 0;
    int cases = 0;
    double worst_rel = 0, worst_gain = 0;
    for (std::size_t i = 0; i < lines.size();) {
        REQUIRE(lines[i][0] == "case");
        const std::string name = lines[i][1];
        const int blocks = std::atoi(lines[i][5].c_str());
        const Start s = parse_start(lines[i + 1]);
        INFO("case " << name);
        const ReferenceCase c(dir / name, name);
        const InterleaverKind kind = kind_from_name(c.manifest().str("interleaver"));
        const Interleaver il = wn0_interleaver(kind);
        REQUIRE(il.size_bits() == s.il_bits);
        rx->reset();
        const fec::ConvolutionalCode code = c.manifest().integer("constraint_length") == 9 ? fec::k9_561_753 : fec::k7_133_171;
        fec::TailBitingViterbiDecoder dec(code, static_cast<std::size_t>(interleaver_3k(0, kind)->input_bits));
        const Result r = run_rx(*rx, dec, c.real32("audio_f32le"), s, il, blocks, 12);
        REQUIRE(r.blocks == blocks);
        const std::size_t symbols = static_cast<std::size_t>(blocks) * s.il_bits / 2;
        REQUIRE(r.dibits.size() == symbols);

        // The decoded bits are the C#'s and the framed transmit bits.
        REQUIRE(lines[i + 2][0] == "info");
        std::string all_info;
        for (const auto& text : r.info) all_info += text;
        CHECK(all_info == lines[i + 2][1]);
        std::string framed;
        for (const std::uint8_t bit : c.bytes("framed_bits")) framed.push_back(static_cast<char>('0' + bit));
        CHECK(all_info == framed);
        decoded_blocks += blocks;

        double scale = 0;
        for (std::size_t k = 0; k < symbols; ++k) scale = std::max({scale, std::fabs(num(lines[i + 3 + k][4])), std::fabs(num(lines[i + 3 + k][3]))});
        for (std::size_t k = 0; k < symbols; ++k) {
            const Tokens& t = lines[i + 3 + k];
            REQUIRE(t[0] == "sym");
            CHECK(r.dibits[k] == std::atoi(t[2].c_str()));
            for (int b = 0; b < 2; ++b) {
                const double err = std::fabs(r.llr[(2 * k) + static_cast<std::size_t>(b)] - num(t[3 + static_cast<std::size_t>(b)]));
                worst_rel = std::max(worst_rel, err / scale);
                CHECK(err <= 1e-4 * scale);
            }
            if (k < 12) {
                REQUIRE(t[5] == "g");
                for (std::size_t f = 0; f < 13; ++f) {
                    const double err = std::fabs(r.gains[k][f] - num(t[6 + f]));
                    worst_gain = std::max(worst_gain, err);
                    CHECK(err <= 0.0015);  // the dump has 3 decimals
                }
            }
        }

        // The decisions are the transmitted di-bits, and past the 8 warm-up symbols so is the sign
        // of every soft value; the deinterleaved block is the wire block through the inverse
        // interleaver, and its signs are the coded bits.
        const auto wire = c.bytes("interleaved_bits");
        const auto coded = c.bytes("punctured_bits");
        for (std::size_t k = 0; k < symbols; ++k) {
            CHECK(r.dibits[k] == truth_dibit(wire, k));
            if (k >= 8) {
                CHECK((r.llr[2 * k] < 0) == (wire[2 * k] == 1));
                CHECK((r.llr[(2 * k) + 1] < 0) == (wire[(2 * k) + 1] == 1));
            }
        }
        std::size_t deint_wrong = 0;
        for (int b = 0; b < blocks; ++b) {
            const auto n = static_cast<std::size_t>(s.il_bits);
            for (std::uint32_t k = 0; k < s.il_bits; ++k) {
                const std::uint32_t loc = il.load_location(k);
                CHECK(r.deint[static_cast<std::size_t>(b)][k] == r.llr[(static_cast<std::size_t>(b) * n) + loc]);
                const bool skip = b == 0 && loc < 16;
                if (!skip && (r.deint[static_cast<std::size_t>(b)][k] < 0) != (coded[(static_cast<std::size_t>(b) * n) + k] == 1)) ++deint_wrong;
            }
        }
        CHECK(deint_wrong == 0);
        i += 3 + symbols;
        ++cases;
    }
    CHECK(cases == 10);
    CHECK(decoded_blocks == 4 + 3 + 3 + 4 + 4 + 3 + 3 + 1 + 1 + 1);
    WARN("worst soft value error " << worst_rel << " of the case maximum, worst finger gain error " << worst_gain);
}

TEST_CASE("reference wn0 receiver agrees with the C# on seeded AWGN and signal loss", "[reference][wn0]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const auto lines = read_dump(MS110D_CSHARP_WN0_AWGN);
    REQUIRE(lines[0][0] == "base");
    const std::string name = lines[0][1];
    const auto lead_in = static_cast<std::size_t>(std::atoi(lines[0][4].c_str()));
    const auto lead_out = static_cast<std::size_t>(std::atoi(lines[0][6].c_str()));
    const auto cut_lead_out = static_cast<std::size_t>(std::atoi(lines[0][8].c_str()));
    const ReferenceCase c(dir / name, name);
    const auto base = c.real32("audio_f32le");
    const auto wire = c.bytes("interleaved_bits");
    const Interleaver il = wn0_interleaver(InterleaverKind::Short);
    auto rx = std::make_unique<StandaloneWn0Receiver>();
    configured_rx(*rx);
    fec::TailBitingViterbiDecoder dec(fec::k7_133_171, 40);

    // The same payload through the C++ transmitter: audio within 1e-6 of the C# audio.
    const auto payload = c.bytes("payload_bits");
    TxSettings ts;
    ts.waveform_number = 0;
    ts.interleaver = InterleaverKind::Short;
    std::size_t tx_bytes = 0;
    REQUIRE(Transmitter::storage_bytes(ts, payload.size(), tx_bytes) == Status::Ok);
    std::vector<std::uint8_t> tx_mem(tx_bytes);
    Transmitter tx;
    REQUIRE(tx.configure(ts, payload.size(), tx_mem) == Status::Ok);
    std::vector<float> cpp_base(base.size());
    std::size_t written = 0;
    REQUIRE(tx.modulate(payload, cpp_base, written) == Status::Ok);
    REQUIRE(written == base.size());

    std::map<std::string, Rates> cs, cpp, cpptx;
    std::map<std::string, double> snr;
    double sigma = 0;
    int cut_trials = 0, lost = 0, info_trials = 0;
    constexpr int data_start_sample = 4 * 1728;
    constexpr int block_samples = 4 * 32 * 40;
    for (std::size_t i = 1; i < lines.size();) {
        const Tokens& t = lines[i];
        if (t[0] == "point") {
            snr[t[1]] = num(t[3]);
            sigma = num(t[5]);
            ++i;
            continue;
        }
        REQUIRE(t[0] == "trial");
        const std::string label = t[1];
        const auto seed = static_cast<std::uint64_t>(std::strtoull(t[2].c_str(), nullptr, 10));
        const int cut_blocks = std::atoi(t[6].c_str());
        if (t[4] == "0") {
            ++i;
            continue;
        }
        const bool cut = cut_blocks > 0;
        const Start s = parse_start(lines[i + 1]);
        const std::string& cs_dibits = lines[i + 2][1];
        const std::string& cs_bits = lines[i + 3][1];
        const std::string& cs_info = lines[i + 4][1];
        const std::string& cs_reason = lines[i + 5][1];
        const auto cs_symbols = static_cast<std::size_t>(std::atoi(lines[i + 5][2].c_str()));
        const std::size_t cut_sample = static_cast<std::size_t>(data_start_sample + (block_samples * cut_blocks));
        const std::size_t total = lead_in + base.size() + (cut ? cut_lead_out : lead_out);
        std::vector<float> audio(total), audio2(total);
        SharedNoise noise(seed);
        for (std::size_t k = 0; k < total; ++k) {
            const bool in = k >= lead_in && k < lead_in + base.size() && (!cut || k - lead_in < cut_sample);
            const bool after_cut = cut && k >= lead_in && k - lead_in >= cut_sample;
            const auto nz = static_cast<float>(sigma * noise.next());
            // After a cut: a clean 1950 Hz tone, no noise (the dump's construction).
            const auto tone = after_cut ? static_cast<float>(0.5 * std::cos(2.0 * 3.14159265358979323846 * 1950.0 * static_cast<double>(k - lead_in) / 9600.0)) : 0.0f;
            audio[k] = (in ? base[k - lead_in] : 0.0f) + tone + (after_cut ? 0.0f : nz);
            audio2[k] = (in ? cpp_base[k - lead_in] : 0.0f) + tone + (after_cut ? 0.0f : nz);
        }
        const int blocks = cut ? cut_blocks : 4;
        rx->reset();
        const Result r = run_rx(*rx, dec, audio, s, il, cut ? all_blocks : 4, 0, 4800);
        rx->reset();
        const Result r2 = run_rx(*rx, dec, audio2, s, il, cut ? all_blocks : 4, 0, 4800);
        REQUIRE(r.blocks >= blocks);

        std::string info;
        for (int b = 0; b < blocks; ++b) info += r.info[static_cast<std::size_t>(b)];
        std::string info2;
        for (int b = 0; b < blocks; ++b) info2 += r2.info[static_cast<std::size_t>(b)];
        CHECK(info == cs_info);
        CHECK(info2 == cs_info);
        ++info_trials;

        if (cut) {
            // Where the signal-lost exit ends the burst, and that it does so, exactly as the C#.
            ++cut_trials;
            CHECK(r.dibits.size() == cs_symbols);
            CHECK(r2.dibits.size() == cs_symbols);
            CHECK((r.end == Wn0Step::SignalLost) == (cs_reason == "SignalLost"));
            lost += cs_reason == "SignalLost";
            CHECK(r.dibits.size() >= cs_dibits.size());
        }
        Rates& a = cs[label];
        Rates& b = cpp[label];
        Rates& d = cpptx[label];
        const std::size_t compare = cs_dibits.size();
        const std::size_t true_symbols = 4 * 40;
        if (!cut) {
            ++a.trials;
            ++b.trials;
            ++d.trials;
        }
        for (std::size_t k = 0; k < compare && k < r.dibits.size(); ++k) {
            const int cs_d = cs_dibits[k] - '0';
            b.disagree += cs_d != r.dibits[k];
            d.disagree += cs_d != r2.dibits[k];
            if (cut || k >= true_symbols) continue;
            const int truth = truth_dibit(wire, k);
            ++a.symbols;
            ++b.symbols;
            ++d.symbols;
            a.dibit_errors += cs_d != truth;
            b.dibit_errors += r.dibits[k] != truth;
            d.dibit_errors += r2.dibits[k] != truth;
            for (std::size_t j = 0; j < 2; ++j) {
                const std::size_t w = (2 * k) + j;
                ++a.bits;
                ++b.bits;
                a.bit_errors += (cs_bits[w] == '1') != (wire[w] == 1);
                b.bit_errors += (r.llr[w] < 0) != (wire[w] == 1);
            }
        }
        i += 6;
    }
    // Exact, not statistical: every decision, soft sign and decoded bit is the C#'s.
    for (const auto& [label, a] : cs) {
        const Rates& b = cpp[label];
        const Rates& d = cpptx[label];
        INFO("point " << label << " snr " << snr[label]);
        WARN("point " << label << " (" << snr[label] << " dB): " << a.trials << " bursts, C# dibit error rate " << a.der()
                      << ", C++ " << b.der() << "; C# soft bit error rate " << a.ber() << ", C++ " << b.ber()
                      << "; C++ transmitter audio dibit error rate " << d.der());
        CHECK(b.disagree == 0);
        CHECK(d.disagree == 0);
        CHECK(b.dibit_errors == a.dibit_errors);
        CHECK(d.dibit_errors == a.dibit_errors);
        CHECK(b.bit_errors == a.bit_errors);
        CHECK(a.der() > 0.02);  // the points really are noisy
    }
    WARN(cut_trials << " cut bursts, the C# lost the signal on " << lost);
    CHECK(cs.size() == 2);
    CHECK(lost >= 20);  // the signal-lost exit really is exercised
    CHECK(info_trials >= 150);
}

TEST_CASE("reference wn0 receiver agrees with the C# on Poor channel bursts", "[reference][wn0]") {
    const std::string stem = poor_stem_or_skip();
    const auto lines = read_dump((stem + ".txt").c_str());
    std::ifstream bin(stem + ".i16le", std::ios::binary);
    REQUIRE(bin.good());
    const Interleaver il = wn0_interleaver(InterleaverKind::Short);
    auto rx = std::make_unique<StandaloneWn0Receiver>();
    configured_rx(*rx);
    fec::TailBitingViterbiDecoder dec(fec::k7_133_171, 40);
    std::map<std::string, Rates> cs, cpp;
    std::map<std::string, double> snr;
    double worst = 0;
    int lost = 0, bursts = 0;
    for (std::size_t i = 0; i < lines.size();) {
        const Tokens& t = lines[i];
        if (t[0] == "point") {
            snr[t[1]] = num(t[3]);
            ++i;
            continue;
        }
        REQUIRE(t[0] == "burst");
        const std::string label = t[1];
        if (t[4] == "0") {
            ++i;
            continue;
        }
        const auto offset = static_cast<std::streamoff>(std::atoll(lines[i + 1][2].c_str()));
        const auto samples = static_cast<std::size_t>(std::atoll(lines[i + 1][4].c_str()));
        const Start s = parse_start(lines[i + 2]);
        const std::string& wire_s = lines[i + 3][1];
        const Tokens& ll = lines[i + 4];
        const std::string& cs_dibits = lines[i + 5][1];
        const std::string& cs_info = lines[i + 7][1];
        const std::string& cs_reason = lines[i + 8][1];
        const auto cs_symbols = static_cast<std::size_t>(std::atoi(lines[i + 8][2].c_str()));
        std::vector<std::int16_t> q(samples);
        bin.clear();
        bin.seekg(offset);
        bin.read(reinterpret_cast<char*>(q.data()), static_cast<std::streamsize>(samples * 2));
        REQUIRE(bin.good());
        std::vector<float> audio(samples);
        for (std::size_t k = 0; k < samples; ++k) audio[k] = static_cast<float>(q[k] / 8192.0);
        rx->reset();
        const Result r = run_rx(*rx, dec, audio, s, il, all_blocks, 0, 4800);
        REQUIRE(r.blocks >= 2);
        ++bursts;
        Rates& a = cs[label];
        Rates& b = cpp[label];
        ++a.trials;
        ++b.trials;
        REQUIRE(ll[0] == "llrs");
        double scale = 0;
        for (std::size_t k = 1; k < ll.size(); ++k) scale = std::max(scale, std::fabs(num(ll[k])));
        for (std::size_t k = 0; k + 1 < ll.size(); ++k) {
            const double err = std::fabs(r.llr[k] - num(ll[1 + k]));
            worst = std::max(worst, err / scale);
            CHECK(err <= 1e-4 * scale);
        }
        CHECK(r.info[0] + r.info[1] == cs_info);
        CHECK(r.dibits.size() == cs_symbols);
        CHECK((r.end == Wn0Step::SignalLost) == (cs_reason == "SignalLost"));
        lost += cs_reason == "SignalLost";
        for (std::size_t k = 0; k < cs_dibits.size() && k < r.dibits.size(); ++k) {
            const int cs_d = cs_dibits[k] - '0';
            b.disagree += cs_d != r.dibits[k];
            if (k >= 80) continue;
            const int truth = ((wire_s[2 * k] - '0') << 1) | (wire_s[(2 * k) + 1] - '0');
            ++a.symbols;
            ++b.symbols;
            a.dibit_errors += cs_d != truth;
            b.dibit_errors += r.dibits[k] != truth;
            for (std::size_t j = 0; j < 2; ++j) {
                const std::size_t w = (2 * k) + j;
                ++a.bits;
                ++b.bits;
                a.bit_errors += (num(ll[1 + w]) < 0) != (wire_s[w] == '1');
                b.bit_errors += (r.llr[w] < 0) != (wire_s[w] == '1');
            }
        }
        i += 9;
    }
    for (const auto& [label, a] : cs) {
        const Rates& b = cpp[label];
        WARN("Poor point " << label << " (" << snr[label] << " dB): " << a.trials << " bursts, dibit error rate C# " << a.der()
                           << " C++ " << b.der() << ", soft bit error rate C# " << a.ber() << " C++ " << b.ber()
                           << ", worst soft value error " << worst);
        CHECK(b.disagree == 0);
        CHECK(b.dibit_errors == a.dibit_errors);
        CHECK(b.bit_errors == a.bit_errors);
        CHECK(a.der() > 0.02);
    }
    WARN(bursts << " bursts, the C# lost the signal on " << lost);
    CHECK(cs.size() == 2);
    CHECK(bursts >= 12);
}

TEST_CASE("wn0 receiver decodes the C++ transmitter, clean and in noise as well as the C# audio", "[wn0]") {
    TxSettings ts;
    ts.waveform_number = 0;
    ts.interleaver = InterleaverKind::Short;
    std::mt19937 gen(5);
    std::vector<std::uint8_t> payload(128);
    for (auto& b : payload) b = static_cast<std::uint8_t>(gen() & 1u);
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(ts, payload.size(), bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(ts, payload.size(), mem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(payload.size(), n) == Status::Ok);
    std::vector<float> audio(n);
    REQUIRE(tx.modulate(payload, audio, n) == Status::Ok);

    // The transmitted wire bits, by the library's own coding stages.
    const auto p = *interleaver_3k(0, InterleaverKind::Short);
    const Interleaver il = wn0_interleaver(InterleaverKind::Short);
    const auto input = static_cast<std::size_t>(p.input_bits);
    std::vector<std::uint8_t> framed(framed_block_count(payload.size(), true, input) * input);
    REQUIRE(build_tx_bits(payload, true, input, framed));
    std::vector<std::uint8_t> wire;
    for (std::size_t b = 0; b < framed.size() / input; ++b) {
        std::vector<std::uint8_t> coded(2 * input), punct(2 * input), fetched(il.size_bits());
        REQUIRE(fec::encode_tail_biting(fec::k7_133_171, std::span(framed).subspan(b * input, input), coded));
        const PunctureSpec* spec = find_puncture(fec::k7_133_171, 1, 2);
        REQUIRE(spec != nullptr);
        REQUIRE(apply_puncture(*spec, coded, punct).has_value());
        il.interleave(punct.data(), fetched.data());
        wire.insert(wire.end(), fetched.begin(), fetched.end());
    }
    const std::size_t symbols = wire.size() / 2;

    // No channel: the burst starts where the transmit chain puts it (chip 0 peaks at half-chip
    // 31.5), with no carrier offset. Every decision is right.
    Start s;
    s.chip0 = 31.5;
    s.data_start = 1728;
    s.il_bits = il.size_bits();
    auto rx = std::make_unique<StandaloneWn0Receiver>();
    configured_rx(*rx);
    fec::TailBitingViterbiDecoder dec(fec::k7_133_171, 40);
    const Result clean = run_rx(*rx, dec, audio, s, il, 4);
    REQUIRE(clean.blocks == 4);
    for (std::size_t k = 0; k < symbols; ++k) CHECK(clean.dibits[k] == truth_dibit(wire, k));
    {
        std::string decoded, sent;
        for (const auto& text : clean.info) decoded += text;
        for (const std::uint8_t bit : framed) sent.push_back(static_cast<char>('0' + bit));
        CHECK(decoded == sent);
    }

    // In noise, at the points of the C# AWGN corpus, the error rates are those of the C# audio.
    const auto lines = read_dump(MS110D_CSHARP_WN0_AWGN);
    double power = 0;
    for (const float x : audio) power += static_cast<double>(x) * static_cast<double>(x);
    power /= static_cast<double>(audio.size());
    std::map<std::string, double> snr_of;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i][0] == "point") snr_of[lines[i][1]] = num(lines[i][3]);
    }
    for (const auto& [label, db] : snr_of) {
        const double sigma = std::sqrt(power / std::pow(10.0, db / 10.0) * 4800.0 / 3000.0);
        Rates r;
        for (int trial = 0; trial < 100; ++trial) {
            SharedNoise noise(50000 + static_cast<std::uint64_t>(trial));
            std::vector<float> a(2400 + audio.size() + 4800, 0.0f);
            for (std::size_t k = 0; k < a.size(); ++k) {
                const float b = k >= 2400 && k < 2400 + audio.size() ? audio[k - 2400] : 0.0f;
                a[k] = b + static_cast<float>(sigma * noise.next());
            }
            Start ns = s;
            ns.chip0 = 31.5 + 1200;
            rx->reset();
            const Result res = run_rx(*rx, dec, a, ns, il, 4, 0, 4800);
            REQUIRE(res.blocks == 4);
            for (std::size_t k = 0; k < symbols; ++k) {
                ++r.symbols;
                r.dibit_errors += res.dibits[k] != truth_dibit(wire, k);
            }
        }
        WARN("C++ transmitter at " << db << " dB: dibit error rate " << r.der());
        // The Walsh dibit error rate at -6 and -9 dB is far from 0 and far from 0.75 (random).
        CHECK(r.der() > 0.02);
        CHECK(r.der() < 0.6);
    }
}

TEST_CASE("wn0 receiver allocates nothing and handles odd calls", "[wn0]") {
    auto rx = std::make_unique<StandaloneWn0Receiver>();
    CHECK_FALSE(rx->configure(0.0));
    CHECK_FALSE(rx->configured());
    std::vector<real> block(80);
    Wn0Start w;
    w.interleaver = Interleaver(80, 0);
    w.data_start_chip = 100;
    CHECK_FALSE(rx->start(w, block));  // not configured
    REQUIRE(rx->configure(0.35));
    CHECK(rx->step() == Wn0Step::Inactive);
    CHECK_FALSE(rx->start(w, block));  // increment 0 is not a permutation
    w.interleaver = wn0_interleaver(InterleaverKind::Short);
    std::vector<real> small(79);
    CHECK_FALSE(rx->start(w, small));
    w.data_start_chip = 2;
    CHECK_FALSE(rx->start(w, block));
    w.data_start_chip = 100;

    std::vector<float> audio(40000);
    SharedNoise gauss(3);
    for (auto& x : audio) x = static_cast<float>(0.3 * gauss.next());
    std::vector<real> out(80);
    std::vector<std::uint8_t> decoded(40);
    fec::TailBitingViterbiDecoder dec(fec::k7_133_171, 40);
    const ms110d_test::AllocationScope scope;
    const bool configured = rx->configure(0.35);
    rx->reset();
    const bool started = rx->start(w, block);
    std::size_t at = 0;
    Wn0Step last = Wn0Step::NeedMoreAudio;
    int symbols = 0;
    while (at < audio.size() && last != Wn0Step::SignalLost && rx->active()) {
        rx->push_audio(std::span<const float>(audio).subspan(at, 500));
        at += 500;
        for (;;) {
            last = rx->step();
            if (last == Wn0Step::Symbol) ++symbols;
            if (last == Wn0Step::BlockComplete) {
                ++symbols;
                rx->deinterleaved_block(out);
                rx->decode_block(dec, decoded);
            }
            if (last != Wn0Step::Symbol && last != Wn0Step::BlockComplete) break;
        }
    }
    const std::size_t allocations = scope.allocations();
    CHECK(configured);
    CHECK(started);
    CHECK(allocations == 0);
    CHECK(symbols > 90);
}
