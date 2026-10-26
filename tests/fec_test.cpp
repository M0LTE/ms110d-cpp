// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fec/convolutional.hpp"
#include "puncture.hpp"

#include "alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace m0lte;

// Exact vectors from the C# reference, M0LTE.Ms110d 0.3.0 (which pulls in M0LTE.Fec), made
// with: dotnet run -c Release --project tools/csharp-vectors -- fec > fec.txt, then
// python3 tools/csharp-vectors/make_fec_inc.py fec.txt > tests/fec_vectors.inc. The program
// calls TailBitingEncoder.Encode, Ms110dPuncture.Get and Ms110dPuncture.Apply. Payload bits
// come from xorshift32 seeded with the seed in the table: for each bit, x ^= x << 13;
// x ^= x >> 17; x ^= x << 5; bit = (x >> 16) & 1. Checksums are FNV-1a 64 over the output
// as one 0/1 byte per bit. The full vectors are the bits packed four per hex digit, first
// bit in the top of the first digit, the last digit zero padded.
#include "fec_vectors.inc"

namespace {

std::vector<std::uint8_t> payload(std::uint32_t seed, std::size_t n) {
    std::vector<std::uint8_t> b(n);
    std::uint32_t x = seed;
    for (auto& v : b) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v = static_cast<std::uint8_t>((x >> 16) & 1u);
    }
    return b;
}

std::uint64_t fnv(const std::vector<std::uint8_t>& b) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (auto v : b) {
        h ^= v;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string hex(const std::vector<std::uint8_t>& bits) {
    std::string s;
    for (std::size_t i = 0; i < bits.size(); i += 4) {
        unsigned v = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            v = (v << 1) | (i + j < bits.size() ? bits[i + j] : 0u);
        }
        s.push_back("0123456789abcdef"[v]);
    }
    return s;
}

const fec::ConvolutionalCode& code_for(int k) { return k == 7 ? fec::k7_133_171 : fec::k9_561_753; }

std::vector<std::uint8_t> encode(const fec::ConvolutionalCode& c, const std::vector<std::uint8_t>& info) {
    std::vector<std::uint8_t> coded(2 * info.size());
    REQUIRE(fec::encode_tail_biting(c, info, coded));
    return coded;
}

}  // namespace

TEST_CASE("code definitions match Figures D-9 and D-10", "[fec]") {
    CHECK(fec::k7_133_171.k == 7);
    CHECK(fec::k7_133_171.poly_t1 == 0133u);
    CHECK(fec::k7_133_171.poly_t2 == 0171u);
    CHECK(fec::k7_133_171.states() == 64u);
    CHECK(fec::k9_561_753.k == 9);
    CHECK(fec::k9_561_753.poly_t1 == 0561u);
    CHECK(fec::k9_561_753.poly_t2 == 0753u);
    CHECK(fec::k9_561_753.states() == 256u);
}

TEST_CASE("encoder: impulse response is the polynomials, T1 first", "[fec]") {
    // A single 1 in an otherwise zero block, placed so that the tail-biting start does
    // not wrap it: output pair i holds input bit (i + k - 1) mod n, so put the 1 at the
    // index entering at pair 0, i = k-1. Pairs 0..k-1 then read the generator from its
    // most significant tap down to x^0.
    for (const auto* c : {&fec::k7_133_171, &fec::k9_561_753}) {
        const std::size_t n = 40;
        std::vector<std::uint8_t> info(n, 0);
        info[static_cast<std::size_t>(c->k) - 1] = 1;
        const auto coded = encode(*c, info);
        // Pair 0 sees only the new bit in the register's lowest cell: tap x^0.
        // Pair j sees it shifted up j places: tap x^j.
        for (int j = 0; j < c->k; ++j) {
            CHECK(coded[2 * static_cast<std::size_t>(j)] == ((c->poly_t1 >> j) & 1u));
            CHECK(coded[2 * static_cast<std::size_t>(j) + 1] == ((c->poly_t2 >> j) & 1u));
        }
    }
}

TEST_CASE("encoder: tail-biting properties", "[fec]") {
    for (const auto* c : {&fec::k7_133_171, &fec::k9_561_753}) {
        const std::size_t n = 57;
        const auto info = payload(99, n);
        const auto a = encode(*c, info);
        CHECK(a.size() == 2 * n);
        // Rotating the input by r rotates the output by 2r (the start state is the same
        // cyclic neighbourhood).
        for (std::size_t r : {1u, 5u, 30u}) {
            std::vector<std::uint8_t> rot(n);
            for (std::size_t i = 0; i < n; ++i) rot[i] = info[(i + r) % n];
            const auto b = encode(*c, rot);
            for (std::size_t i = 0; i < 2 * n; ++i) {
                CHECK(b[i] == a[(i + 2 * r) % (2 * n)]);
            }
        }
        // All zeros give all zeros.
        CHECK(fnv(encode(*c, std::vector<std::uint8_t>(n, 0))) == fnv(std::vector<std::uint8_t>(2 * n, 0)));
    }
}

TEST_CASE("encoder rejects bad sizes", "[fec]") {
    std::vector<std::uint8_t> info(10, 1), coded(20), shortc(19);
    CHECK(fec::encode_tail_biting(fec::k7_133_171, info, coded));
    CHECK_FALSE(fec::encode_tail_biting(fec::k7_133_171, info, shortc));
    std::vector<std::uint8_t> tiny(8, 1), tiny_coded(16);
    CHECK_FALSE(fec::encode_tail_biting(fec::k9_561_753, tiny, tiny_coded));  // block shorter than k
    CHECK(fec::encode_tail_biting(fec::k7_133_171, tiny, tiny_coded));
}

TEST_CASE("every Table D-L rate: masks give the code rate", "[puncture]") {
    // (ones in both rows) * ... : per mask cycle, columns * repeat... gives
    // info bits = columns / repeat and output bits = ones, so
    // columns * den == ones * num * repeat.
    const unsigned rates[][2] = {{9, 10}, {8, 9}, {5, 6}, {4, 5}, {3, 4}, {2, 3}, {4, 7}, {9, 16}, {1, 2},
                                 {2, 5},  {1, 3}, {2, 7}, {1, 4}, {1, 6}, {1, 8}, {1, 12}, {1, 16}};
    for (const auto* c : {&fec::k7_133_171, &fec::k9_561_753}) {
        for (const auto& r : rates) {
            const auto* s = ms110d::find_puncture(*c, r[0], r[1]);
            REQUIRE(s != nullptr);
            unsigned ones = 0;
            for (unsigned i = 0; i < s->columns; ++i) ones += ((s->keep_t1 >> i) & 1u) + ((s->keep_t2 >> i) & 1u);
            INFO("K" << c->k << " " << r[0] << "/" << r[1]);
            CHECK(s->rate_num == r[0]);
            CHECK(s->rate_den == r[1]);
            CHECK(s->columns * r[1] == ones * r[0] * s->repeat);
            CHECK(s->keep_t1 >> s->columns == 0);
            CHECK(s->keep_t2 >> s->columns == 0);
        }
    }
    CHECK(ms110d::find_puncture(fec::k7_133_171, 7, 8) == nullptr);
    CHECK(ms110d::find_puncture(fec::ConvolutionalCode{5, 023, 035}, 3, 4) == nullptr);
}

TEST_CASE("puncture length and capacity", "[puncture]") {
    const auto* s = ms110d::find_puncture(fec::k7_133_171, 3, 4);
    REQUIRE(s != nullptr);
    CHECK(ms110d::punctured_length(*s, 30) == 40);
    CHECK_FALSE(ms110d::punctured_length(*s, 31).has_value());
    std::vector<std::uint8_t> coded(60, 1), small(39), exact(40);
    CHECK_FALSE(ms110d::apply_puncture(*s, coded, small).has_value());
    CHECK(ms110d::apply_puncture(*s, coded, exact) == 40);
}

TEST_CASE("encoder matches the C# reference (checksums)", "[fec][csharp]") {
    for (const auto& v : encode_vectors) {
        INFO("K" << v.k << " n=" << v.n);
        CHECK(fnv(encode(code_for(v.k), payload(v.seed, v.n))) == v.fnv);
    }
}

TEST_CASE("encoder and puncturing match the C# reference (checksums, every rate)", "[puncture][csharp]") {
    for (const auto& v : puncture_vectors) {
        INFO("K" << v.k << " " << v.num << "/" << v.den << " n=" << v.n);
        const auto& c = code_for(v.k);
        const auto* s = ms110d::find_puncture(c, v.num, v.den);
        REQUIRE(s != nullptr);
        const auto coded = encode(c, payload(v.seed, v.n));
        REQUIRE(ms110d::punctured_length(*s, v.n) == v.out_len);
        std::vector<std::uint8_t> out(v.out_len);
        CHECK(ms110d::apply_puncture(*s, coded, out) == v.out_len);
        CHECK(fnv(out) == v.fnv);
    }
}

TEST_CASE("full vectors from the C# reference", "[puncture][csharp]") {
    for (const auto& v : full_vectors) {
        INFO("K" << v.k << " " << v.num << "/" << v.den);
        const auto& c = code_for(v.k);
        const auto info = payload(v.seed, v.n);
        CHECK(hex(info) == v.info);
        const auto coded = encode(c, info);
        CHECK(hex(coded) == v.coded);
        const auto* s = ms110d::find_puncture(c, v.num, v.den);
        REQUIRE(s != nullptr);
        std::vector<std::uint8_t> out(*ms110d::punctured_length(*s, v.n));
        REQUIRE(ms110d::apply_puncture(*s, coded, out).has_value());
        CHECK(hex(out) == v.punctured);
    }
}

TEST_CASE("encode and puncture do not allocate", "[fec][alloc]") {
    const auto info = payload(5, 360);
    std::vector<std::uint8_t> coded(720), out(5760);
    const auto* s = ms110d::find_puncture(fec::k9_561_753, 1, 16);
    REQUIRE(s != nullptr);
    const ms110d_test::AllocationScope scope;
    const bool ok = fec::encode_tail_biting(fec::k9_561_753, info, coded);
    const auto w = ms110d::apply_puncture(*s, coded, out);
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(ok);
    CHECK(w == 5760);
}
