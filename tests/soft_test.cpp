// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The soft decoders against the C# reference (M0LTE.Ms110d 0.2.0): the tail-biting Viterbi and
// SISO decoders, the max-log BCJR and the chain BCJR. The expected values are in
// tests/data/csharp-soft-0.2.0.txt, made by `dotnet run -c Release --project tools/csharp-vectors
// -- soft` (see tools/csharp-vectors/SoftDump.cs). Inputs are regenerated here from the same
// integer generator and checked against an FNV-1a checksum of their bits, so a drifting
// generator is caught as such. See docs/dev/soft-decoders.md for the tolerances and why.

#include "bcjr.hpp"
#include "fec/convolutional.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"

#include "alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef MS110D_CSHARP_SOFT_DUMP
#error "MS110D_CSHARP_SOFT_DUMP must be defined by the build"
#endif

using namespace m0lte;
using ms110d::real;
using Cf = std::complex<float>;
using Cr = std::complex<real>;

namespace {

// ---- The generator shared with tools/csharp-vectors/SoftDump.cs --------------------------

class Rng {
public:
    explicit Rng(std::uint32_t seed) : x_(seed) {}
    int u16() { return static_cast<int>(next() >> 16); }
    int bit() { return u16() & 1; }
    double gauss() {
        int s = 0;
        for (int i = 0; i < 12; ++i) s += u16();
        return static_cast<double>(s - 393216) / 65536.0;
    }

private:
    std::uint32_t next() {
        x_ ^= x_ << 13;
        x_ ^= x_ >> 17;
        x_ ^= x_ << 5;
        return x_;
    }
    std::uint32_t x_;
};

class Fnv {
public:
    void add(std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h_ ^= (v >> (8 * b)) & 0xFFu;
            h_ *= 0x100000001b3ull;
        }
    }
    void add(float f) {
        std::uint32_t v;
        std::memcpy(&v, &f, 4);
        add(v);
    }
    void add(std::uint8_t v) {
        h_ ^= v;
        h_ *= 0x100000001b3ull;
    }
    void add(Cf c) {
        add(c.real());
        add(c.imag());
    }
    template <typename T>
    void add_all(const std::vector<T>& v) {
        for (const auto& e : v) add(e);
    }
    std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};

struct Block {
    std::vector<std::uint8_t> info;
    std::vector<std::uint8_t> coded;
    std::vector<float> llr;
};

Block make_block(const fec::ConvolutionalCode& code, std::size_t n, double sigma, Rng& rng) {
    Block b;
    b.info.resize(n);
    for (auto& v : b.info) v = static_cast<std::uint8_t>(rng.bit());
    b.coded.resize(2 * n);
    REQUIRE(fec::encode_tail_biting(code, b.info, b.coded));
    b.llr.resize(2 * n);
    for (std::size_t i = 0; i < 2 * n; ++i) {
        const double y = (1.0 - (2.0 * b.coded[i])) + (sigma * rng.gauss());
        b.llr[i] = static_cast<float>((2.0 * y) / (sigma * sigma));
    }
    return b;
}

// ---- Dump parsing ------------------------------------------------------------------------

using Line = std::vector<std::string>;

const std::vector<Line>& dump() {
    static const std::vector<Line> lines = [] {
        std::ifstream in(MS110D_CSHARP_SOFT_DUMP);
        std::vector<Line> out;
        std::string text;
        while (std::getline(in, text)) {
            if (text.empty() || text[0] == '#') continue;
            std::istringstream ss(text);
            Line f;
            std::string tok;
            while (ss >> tok) f.push_back(tok);
            out.push_back(f);
        }
        return out;
    }();
    return lines;
}

std::vector<Line> lines_of(const std::string& tag) {
    std::vector<Line> out;
    for (const auto& l : dump()) {
        if (l[0] == tag) out.push_back(l);
    }
    REQUIRE(!out.empty());
    return out;
}

std::vector<float> floats_from(const Line& l, std::size_t first) {
    std::vector<float> v;
    for (std::size_t i = first; i < l.size(); ++i) {
        const std::uint32_t bits = static_cast<std::uint32_t>(std::stoul(l[i], nullptr, 16));
        float f;
        std::memcpy(&f, &bits, 4);
        v.push_back(f);
    }
    return v;
}

std::vector<std::uint8_t> bits_from_hex(const std::string& hex, std::size_t n) {
    std::vector<std::uint8_t> bits(n);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = hex[i / 4];
        const unsigned v = static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        bits[i] = static_cast<std::uint8_t>((v >> (3 - i % 4)) & 1u);
    }
    return bits;
}

const fec::ConvolutionalCode& code_for(const std::string& name) {
    return name == "K7" ? fec::k7_133_171 : fec::k9_561_753;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    }
    return worst;
}

// Largest |a - b| / (1 + |b|) over the vectors.
template <typename A>
double max_rel_diff(const std::vector<A>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        worst = std::max(worst, d / (1.0 + std::fabs(static_cast<double>(b[i]))));
    }
    return worst;
}

// Tolerances, justified in docs/dev/soft-decoders.md. The SISO and Viterbi are the same float
// arithmetic in the same order as the C#; measured agreement is bit for bit on glibc, and the
// bound only has to absorb a libm that rounds the 128 table entries one ulp differently.
constexpr double siso_tolerance = 1e-3;
// BCJR, as |a - b| / (1 + |b|): the single build runs the C# arithmetic (measured 0, bound allows
// a libm that rounds log and exp one ulp apart); the double build differs by the C#'s float
// rounding (measured 3e-5 worst, bound 1e-4).
constexpr double bcjr_tolerance = sizeof(real) == 4 ? 1e-6 : 1e-4;

}  // namespace

TEST_CASE("Viterbi and SISO agree with the C# on seeded noisy blocks", "[soft][csharp]") {
    double worst_post = 0;
    std::size_t cases = 0;
    for (const auto& l : lines_of("DEC")) {
        // DEC code n variant sigma seed in-fnv viterbi-bits siso-posteriors...
        const auto& code = code_for(l[1]);
        const std::size_t n = std::stoul(l[2]);
        const std::string& variant = l[3];
        const double sigma = std::stod(l[4]);
        Rng rng(static_cast<std::uint32_t>(std::stoul(l[5])));
        Block b = make_block(code, n, sigma, rng);
        if (variant == "erased") {
            for (std::size_t i = 0; i < b.llr.size(); ++i) {
                if (i % 6 == 1 || i % 6 == 2) b.llr[i] = 0.0f;
            }
        }
        if (variant == "zero") std::fill(b.llr.begin(), b.llr.end(), 0.0f);

        Fnv f;
        f.add_all(b.llr);
        REQUIRE(f.value() == std::stoull(l[6], nullptr, 16));

        fec::TailBitingViterbiDecoder vit(code, n);
        std::vector<std::uint8_t> bits(n);
        REQUIRE(vit.decode(b.llr, bits));
        CHECK(bits == bits_from_hex(l[7], n));

        fec::TailBitingSisoDecoder siso(code, n);
        std::vector<float> post(2 * n);
        REQUIRE(siso.decode(b.llr, post));
        const auto expected = floats_from(l, 8);
        const double d = max_abs_diff(post, expected);
        worst_post = std::max(worst_post, d);
        CHECK(d <= siso_tolerance);
        ++cases;
    }
    std::printf("[soft] %zu decoder cases; worst SISO posterior difference %.3g\n", cases, worst_post);
}

TEST_CASE("bit error rate curves match the C# exactly", "[soft][csharp]") {
    // The same blocks (generated from the same seeds) go through the Viterbi and the SISO in
    // C++; the error counts and checksums of every decoded bit must equal the C#'s. Prints the
    // curve (Eb/N0 = 1 / sigma^2 for rate 1/2 BPSK).
    std::printf("[soft] %-4s %-8s %-9s %-14s %-14s\n", "code", "Eb/N0", "blocks", "info BER vit", "coded BER siso");
    double previous_vit = 1.0;
    std::string previous_code;
    for (const auto& l : lines_of("BER")) {
        // BER code n sigma seed blocks info-err coded-err in-fnv vit-fnv siso-fnv
        const auto& code = code_for(l[1]);
        const std::size_t n = std::stoul(l[2]);
        const double sigma = std::stod(l[3]);
        Rng rng(static_cast<std::uint32_t>(std::stoul(l[4])));
        const std::size_t blocks = std::stoul(l[5]);

        fec::TailBitingViterbiDecoder vit(code, n);
        fec::TailBitingSisoDecoder siso(code, n);
        std::vector<std::uint8_t> bits(n);
        std::vector<float> post(2 * n);
        std::size_t info_err = 0, coded_err = 0;
        Fnv fin, fv, fs;
        for (std::size_t k = 0; k < blocks; ++k) {
            const Block b = make_block(code, n, sigma, rng);
            fin.add_all(b.llr);
            REQUIRE(vit.decode(b.llr, bits));
            REQUIRE(siso.decode(b.llr, post));
            for (std::size_t i = 0; i < n; ++i) {
                if (bits[i] != b.info[i]) ++info_err;
                fv.add(bits[i]);
            }
            for (std::size_t i = 0; i < 2 * n; ++i) {
                const std::uint8_t hard = post[i] > 0.0f ? 0 : 1;
                if (hard != b.coded[i]) ++coded_err;
                fs.add(hard);
            }
        }
        CHECK(fin.value() == std::stoull(l[8], nullptr, 16));
        CHECK(info_err == std::stoul(l[6]));
        CHECK(coded_err == std::stoul(l[7]));
        CHECK(fv.value() == std::stoull(l[9], nullptr, 16));
        CHECK(fs.value() == std::stoull(l[10], nullptr, 16));

        const double nbits = static_cast<double>(blocks * n);
        const double ber_v = static_cast<double>(info_err) / nbits;
        const double ber_s = static_cast<double>(coded_err) / (2 * nbits);
        std::printf("[soft] %-4s %6.2f dB %-9zu %-14.3e %-14.3e (C# %zu, %zu errors)\n", l[1].c_str(),
                    10.0 * std::log10(1.0 / (sigma * sigma)), blocks, ber_v, ber_s,
                    static_cast<std::size_t>(std::stoul(l[6])), static_cast<std::size_t>(std::stoul(l[7])));
        // The curve falls as the noise falls (per code; the lines are in falling sigma order).
        if (l[1] == previous_code) CHECK(ber_v <= previous_vit);
        previous_code = l[1];
        previous_vit = ber_v;
    }
}

namespace {

// Channel generator of SoftDump.Channel. xs is empty for BPSK with pre block symbols +1.
struct Channel {
    std::vector<Cf> rx, h1, h2;
};

Channel make_channel(Rng& rng, std::size_t n, std::size_t delay, double sigma, const std::vector<Cf>* cons,
                     const std::vector<int>* xs) {
    Channel ch;
    ch.rx.resize(n);
    ch.h1.resize(n);
    ch.h2.resize(n);
    std::vector<double> xr(n + delay), xi(n + delay);
    for (std::size_t i = 0; i < n + delay; ++i) {
        if (cons == nullptr) {
            xr[i] = i < delay ? 1.0 : 1.0 - (2.0 * rng.bit());
            xi[i] = 0.0;
        } else {
            xr[i] = static_cast<double>((*cons)[static_cast<std::size_t>((*xs)[i])].real());
            xi[i] = static_cast<double>((*cons)[static_cast<std::size_t>((*xs)[i])].imag());
        }
    }
    for (std::size_t t = 0; t < n; ++t) {
        const double u = static_cast<double>((t * 7) % 13) / 13.0;
        const double a1r = 1.0 - (0.3 * u), a1i = 0.2 * u;
        const double a2r = 0.5 - (0.2 * u), a2i = -0.3 + (0.4 * u);
        const double x0r = xr[t + delay], x0i = xi[t + delay], x1r = xr[t], x1i = xi[t];
        double yr = (a1r * x0r) - (a1i * x0i) + ((a2r * x1r) - (a2i * x1i));
        double yi = (a1r * x0i) + (a1i * x0r) + ((a2r * x1i) + (a2i * x1r));
        yr += sigma * rng.gauss();
        yi += sigma * rng.gauss();
        ch.rx[t] = Cf(static_cast<float>(yr), static_cast<float>(yi));
        ch.h1[t] = Cf(static_cast<float>(a1r), static_cast<float>(a1i));
        ch.h2[t] = Cf(static_cast<float>(a2r), static_cast<float>(a2i));
    }
    return ch;
}

std::vector<Cr> widen(const std::vector<Cf>& v) {
    std::vector<Cr> out;
    for (const auto& c : v) out.emplace_back(static_cast<real>(c.real()), static_cast<real>(c.imag()));
    return out;
}

void constellation(std::size_t m, std::vector<Cf>& cons, std::vector<std::uint8_t>& labels) {
    constexpr float c = 0.70710678f;
    labels.clear();
    if (m == 2) {
        cons = {Cf(1.0f, 0.0f), Cf(-1.0f, 0.0f)};
    } else if (m == 4) {
        cons = {Cf(c, c), Cf(-c, c), Cf(-c, -c), Cf(c, -c)};
        labels = {0, 1, 3, 2};
    } else {
        cons = {Cf(1.0f, 0.0f), Cf(c, c), Cf(0.0f, 1.0f), Cf(-c, c), Cf(-1.0f, 0.0f), Cf(-c, -c), Cf(0.0f, -1.0f), Cf(c, -c)};
        labels = {0, 1, 3, 2, 7, 6, 4, 5};
    }
}

void add_channel(Fnv& f, const Channel& ch) {
    for (const auto& c : ch.rx) f.add(c);
    for (const auto& c : ch.h1) f.add(c);
    for (const auto& c : ch.h2) f.add(c);
}

}  // namespace

TEST_CASE("max-log BCJR agrees with the C#", "[soft][csharp]") {
    double worst = 0;
    for (const auto& l : lines_of("BCJR")) {
        // BCJR delay n sigma seed in-fnv llrs...
        const int delay = std::stoi(l[1]);
        const std::size_t n = std::stoul(l[2]);
        const double sigma = std::stod(l[3]);
        Rng rng(static_cast<std::uint32_t>(std::stoul(l[4])));
        const Channel ch = make_channel(rng, n, static_cast<std::size_t>(delay), sigma, nullptr, nullptr);
        Fnv f;
        add_channel(f, ch);
        REQUIRE(f.value() == std::stoull(l[5], nullptr, 16));

        const auto rx = widen(ch.rx), h1 = widen(ch.h1), h2 = widen(ch.h2);
        ms110d::Bcjr bcjr(n, 6);
        std::vector<real> llrs(n);
        REQUIRE(bcjr.equalize(rx, h1, h2, delay, static_cast<real>(static_cast<float>(sigma * sigma)), llrs));
        const double d = max_rel_diff(llrs, floats_from(l, 6));
        worst = std::max(worst, d);
        CHECK(d <= bcjr_tolerance);
    }
    std::printf("[soft] max-log BCJR worst difference %.3g (|a-b|/(1+|b|), %s precision)\n", worst,
                sizeof(real) == 4 ? "single" : "double");
}

TEST_CASE("chain BCJR agrees with the C#", "[soft][csharp]") {
    double worst = 0;
    for (const auto& l : lines_of("CHAIN")) {
        // CHAIN m delay n flags sigma seed in-fnv llrs...
        const std::size_t m = std::stoul(l[1]);
        const std::size_t delay = std::stoul(l[2]);
        const std::size_t n = std::stoul(l[3]);
        const int flags = std::stoi(l[4]);
        const double sigma = std::stod(l[5]);
        Rng rng(static_cast<std::uint32_t>(std::stoul(l[6])));
        std::vector<Cf> cons;
        std::vector<std::uint8_t> labels;
        constellation(m, cons, labels);
        const std::size_t bps = m == 2 ? 1 : m == 4 ? 2 : 3;
        std::vector<int> xs(n + delay);
        for (auto& x : xs) x = rng.u16() % static_cast<int>(m);
        const Channel ch = make_channel(rng, n, delay, sigma, &cons, &xs);
        Fnv f;
        add_channel(f, ch);
        std::vector<Cf> preceding;
        if ((flags & 4) == 0) {
            for (std::size_t k = 0; k < delay; ++k) preceding.push_back(cons[static_cast<std::size_t>(xs[k])]);
        }
        std::vector<real> priors, nvps;
        if ((flags & 1) != 0) {
            std::vector<float> p(n * m);
            for (auto& v : p) v = static_cast<float>(-(rng.u16() % 2000) / 250.0);
            f.add_all(p);
            for (float v : p) priors.push_back(static_cast<real>(v));
        }
        if ((flags & 2) != 0) {
            std::vector<float> p(n);
            for (std::size_t t = 0; t < n; ++t) {
                p[t] = static_cast<float>(sigma * sigma * (0.5 + static_cast<double>((t * 5) % 9) / 8.0));
            }
            f.add_all(p);
            for (float v : p) nvps.push_back(static_cast<real>(v));
        }
        REQUIRE(f.value() == std::stoull(l[7], nullptr, 16));

        const auto rx = widen(ch.rx), h1 = widen(ch.h1), h2 = widen(ch.h2);
        ms110d::ChainBcjr chain(n, 8);
        ms110d::ChainBcjr::Input in;
        in.rx = rx;
        in.h1 = h1;
        in.h2 = h2;
        in.delay = static_cast<int>(delay);
        in.noise_var = static_cast<real>(static_cast<float>(sigma * sigma));
        in.constellation = cons;
        in.bit_labels = labels;
        in.bits_per_symbol = static_cast<int>(bps);
        in.preceding = preceding;
        in.symbol_log_priors = priors;
        in.noise_var_per_symbol = nvps;
        std::vector<real> llrs(n * bps);
        REQUIRE(chain.equalize(in, llrs));
        const double d = max_rel_diff(llrs, floats_from(l, 8));
        worst = std::max(worst, d);
        CHECK(d <= bcjr_tolerance);
    }
    std::printf("[soft] chain BCJR worst difference %.3g (|a-b|/(1+|b|), %s precision)\n", worst,
                sizeof(real) == 4 ? "single" : "double");
}

TEST_CASE("decoders: clean channel, argument checks and no allocation", "[soft]") {
    for (const auto* code : {&fec::k7_133_171, &fec::k9_561_753}) {
        const std::size_t n = 64;
        Rng rng(77);
        Block b = make_block(*code, n, 0.05, rng);  // essentially noise free
        fec::TailBitingViterbiDecoder vit(*code, 200);
        fec::TailBitingSisoDecoder siso(*code, 200);
        std::vector<std::uint8_t> bits(n);
        std::vector<float> post(2 * n);

        // Everything below the line runs with the heap watched: not one allocation.
        bool ok_v, ok_s;
        const ms110d_test::AllocationScope scope;
        ok_v = vit.decode(b.llr, bits);
        ok_s = siso.decode(b.llr, post);
        const std::size_t seen = scope.allocations();
        CHECK(seen == 0);
        CHECK(ok_v);
        CHECK(ok_s);
        CHECK(bits == b.info);
        for (std::size_t i = 0; i < 2 * n; ++i) {
            CHECK(((post[i] > 0.0f ? 0 : 1) == b.coded[i]));
        }

        // Bad arguments return false and write nothing.
        std::vector<std::uint8_t> sentinel(n, 7);
        CHECK_FALSE(vit.decode(std::span<const float>(b.llr).first(2 * n - 1), sentinel));
        CHECK(sentinel == std::vector<std::uint8_t>(n, 7));
        std::vector<std::uint8_t> tiny(static_cast<std::size_t>(code->k) - 1);
        std::vector<float> tiny_llr(2 * tiny.size());
        CHECK_FALSE(vit.decode(tiny_llr, tiny));
        std::vector<std::uint8_t> big(201);
        std::vector<float> big_llr(402);
        CHECK_FALSE(vit.decode(big_llr, big));
        std::vector<float> big_post(402);
        CHECK_FALSE(siso.decode(big_llr, big_post));
        std::vector<float> wrong(2 * n + 2);
        CHECK_FALSE(siso.decode(b.llr, wrong));
        CHECK_FALSE(siso.decode(std::span<const float>(b.llr).first(2 * n - 1), std::span<float>(post).first(2 * n - 1)));
        CHECK_FALSE(siso.decode(tiny_llr, tiny_llr));
    }
    CHECK_FALSE(fec::TailBitingViterbiDecoder(fec::ConvolutionalCode{20, 1, 1}, 10).valid());
    CHECK_FALSE(fec::TailBitingSisoDecoder(fec::ConvolutionalCode{1, 1, 1}, 10).valid());
}

TEST_CASE("BCJR equalisers: argument checks and no allocation", "[soft]") {
    const std::size_t n = 50;
    Rng rng(5);
    std::vector<Cf> cons;
    std::vector<std::uint8_t> labels;
    constellation(4, cons, labels);
    std::vector<int> xs(n + 3);
    for (auto& x : xs) x = rng.u16() % 4;
    const Channel ch = make_channel(rng, n, 3, 0.2, &cons, &xs);
    const auto rx = widen(ch.rx), h1 = widen(ch.h1), h2 = widen(ch.h2);

    const real nv = static_cast<real>(0.04f);
    const real zero = 0;
    ms110d::Bcjr bcjr(100, 4);
    ms110d::ChainBcjr chain(100, 8);
    ms110d::ChainBcjr::Input in;
    in.rx = rx;
    in.h1 = h1;
    in.h2 = h2;
    in.delay = 3;
    in.noise_var = nv;
    in.constellation = cons;
    in.bit_labels = labels;
    in.bits_per_symbol = 2;
    std::vector<real> llrs_b(n), llrs_c(2 * n);

    bool ok_b, ok_c;
    const ms110d_test::AllocationScope scope;
    ok_b = bcjr.equalize(rx, h1, h2, 3, nv, llrs_b);
    ok_c = chain.equalize(in, llrs_c);
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(ok_b);
    CHECK(ok_c);

    // A clean QPSK channel at 0.2 sd: the chain decisions recover the transmitted labels.
    std::size_t wrong = 0;
    for (std::size_t t = 0; t < n; ++t) {
        const unsigned label = labels[static_cast<std::size_t>(xs[t + 3])];
        for (std::size_t b = 0; b < 2; ++b) {
            const unsigned bit = (label >> (1 - b)) & 1u;
            if ((llrs_c[2 * t + b] > 0 ? 0u : 1u) != bit) ++wrong;
        }
    }
    CHECK(wrong <= 2);

    CHECK_FALSE(bcjr.equalize(rx, h1, h2, 5, nv, llrs_b));      // delay above the maximum
    CHECK_FALSE(bcjr.equalize(rx, h1, h2, 0, nv, llrs_b));      // delay below one
    CHECK_FALSE(bcjr.equalize(rx, h1, h2, 3, zero, llrs_b));       // no noise variance
    CHECK_FALSE(bcjr.equalize(rx, h1, std::span<const Cr>(h2).first(n - 1), 3, nv, llrs_b));
    CHECK_FALSE(ms110d::Bcjr(10, 30).valid());
    auto bad = in;
    bad.bits_per_symbol = 3;                                       // llrs the wrong length
    CHECK_FALSE(chain.equalize(bad, llrs_c));
    bad = in;
    bad.delay = 0;
    CHECK_FALSE(chain.equalize(bad, llrs_c));
    bad = in;
    std::vector<Cf> short_pre(2);
    bad.preceding = short_pre;                                     // fewer than delay
    CHECK_FALSE(chain.equalize(bad, llrs_c));
    ms110d::ChainBcjr small(10, 8);
    CHECK_FALSE(small.equalize(in, llrs_c));                       // longer than the constructor allows
}

TEST_CASE("decoders survive infinite and NaN LLRs", "[soft]") {
    // An infinite or NaN LLR must not be undefined behaviour (the sanitizer build checks); the
    // outputs are whatever the arithmetic gives.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto* code : {&fec::k7_133_171, &fec::k9_561_753}) {
        const std::size_t n = 40;
        Rng rng(9);
        const Block b = make_block(*code, n, 0.7, rng);
        fec::TailBitingViterbiDecoder vit(*code, n);
        fec::TailBitingSisoDecoder siso(*code, n);
        for (float bad : {inf, -inf, nan}) {
            std::vector<float> llr = b.llr;
            llr[3] = bad;
            llr[10] = -bad;
            llr[11] = bad;
            std::vector<std::uint8_t> bits(n);
            std::vector<float> post(2 * n);
            CHECK(vit.decode(llr, bits));
            CHECK(siso.decode(llr, post));
        }
    }
}

TEST_CASE("Viterbi on a block long enough to renormalise", "[soft]") {
    // 1100 + 2 * 42 steps passes the metric renormalisation at step 1023. With float metrics
    // and LLRs of this size the metrics would not overflow without it, so a mutation that
    // removes the renormalisation cannot be made to fail here; this checks the path runs and
    // decodes correctly (high SNR, so the block must come back exactly).
    const fec::ConvolutionalCode& code = fec::k7_133_171;
    const std::size_t n = 1100;
    Rng rng(31);
    const Block b = make_block(code, n, 0.4, rng);
    fec::TailBitingViterbiDecoder vit(code, n);
    std::vector<std::uint8_t> bits(n);
    REQUIRE(vit.decode(b.llr, bits));
    CHECK(bits == b.info);
}
