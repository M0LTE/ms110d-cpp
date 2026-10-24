// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "../src/interleaver.hpp"
#include "../src/scrambler.hpp"
#include "../src/trinomial_scrambler.hpp"
#include "alloc_guard.hpp"
#include "csharp_vectors.hpp"

#include <cstdint>
#include <vector>

using namespace m0lte::ms110d;

namespace {

struct Fnv {
    std::uint64_t h = 14695981039346656037ULL;
    void add(std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFFu;
            h *= 1099511628211ULL;
        }
    }
};

}  // namespace

TEST_CASE("data scrambler: PSK matches the C# reference", "[scrambler]") {
    Scrambler s;
    Fnv f;
    for (int i = 0; i < 1024; ++i) {
        const int v = s.next_psk(0);
        if (i < 32) CHECK(v == csharp_vectors::psk_head[i]);
        f.add(static_cast<std::uint32_t>(v));
    }
    CHECK(f.h == csharp_vectors::psk_fnv);

    s.reset();
    Fnv m;
    for (int i = 0; i < 1024; ++i) m.add(static_cast<std::uint32_t>(s.next_psk(i % 8)));
    CHECK(m.h == csharp_vectors::psk_mix_fnv);
}

TEST_CASE("data scrambler: QAM matches the C# reference", "[scrambler]") {
    struct Case {
        int bits;
        const int* head;
        std::uint64_t fnv;
        std::uint64_t mix;
    };
    const Case cases[] = {
        {4, csharp_vectors::qam4_head, csharp_vectors::qam4_fnv, csharp_vectors::qam4_mix_fnv},
        {5, csharp_vectors::qam5_head, csharp_vectors::qam5_fnv, csharp_vectors::qam5_mix_fnv},
        {6, csharp_vectors::qam6_head, csharp_vectors::qam6_fnv, csharp_vectors::qam6_mix_fnv},
        {8, csharp_vectors::qam8_head, csharp_vectors::qam8_fnv, csharp_vectors::qam8_mix_fnv},
    };
    for (const Case& c : cases) {
        CAPTURE(c.bits);
        Scrambler s;
        Fnv f;
        for (int i = 0; i < 1024; ++i) {
            const int v = s.next_qam(0, c.bits);
            if (i < 32) CHECK(v == c.head[i]);
            f.add(static_cast<std::uint32_t>(v));
        }
        CHECK(f.h == c.fnv);
        s.reset();
        Fnv m;
        for (int i = 0; i < 1024; ++i) m.add(static_cast<std::uint32_t>(s.next_qam(i & ((1 << c.bits) - 1), c.bits)));
        CHECK(m.h == c.mix);
    }
}

TEST_CASE("data scrambler: standard's properties", "[scrambler][standard]") {
    // D.5.1.3: first symbol uses the initialisation value 1; the sequence is 511 bits long.
    Scrambler s;
    CHECK(s.state() == 1u);
    CHECK(s.next_psk(0) == 1);
    s.reset();
    int period = 0;
    do {
        s.next_qam(0, 1);
        ++period;
    } while (s.state() != 1u && period < 1000);
    CHECK(period == 511);
    // D.5.1.3 modulo-8 sum, with register bits 001: (7 + 1) mod 8 = 0.
    s.reset();
    CHECK(s.next_psk(7) == 0);  // register 001: (7 + 1) mod 8
}

TEST_CASE("Trinomial(159,31) scrambler: the standard's printed sequence", "[scrambler][standard]") {
    // D.5.1.4, "the first 32 symbols of the scramble sequence".
    const int printed[32] = {5, 6, 2, 1, 7, 3, 1, 1, 6, 0, 5, 4, 0, 7, 7, 0,
                             5, 3, 1, 3, 3, 2, 2, 5, 5, 4, 7, 3, 5, 4, 3, 0};
    // The printed worked combine row for data di-bit 01 (Walsh 0 4 0 4 ...).
    const int combined[32] = {5, 2, 2, 5, 7, 7, 1, 5, 6, 4, 5, 0, 0, 3, 7, 4,
                              5, 7, 1, 7, 3, 6, 2, 1, 5, 0, 7, 7, 5, 0, 3, 4};
    Trinomial159Scrambler t;
    for (int i = 0; i < 32; ++i) {
        const int v = t.next();
        CHECK(v == printed[i]);
        CHECK(((v + ((i & 1) ? 4 : 0)) & 7) == combined[i]);
    }
}

TEST_CASE("Trinomial(159,31) scrambler matches the C# reference", "[scrambler]") {
    Trinomial159Scrambler t;
    Fnv all;
    Fnv first;
    for (int i = 0; i < 4200; ++i) {
        const int v = t.next();
        if (i < 64) CHECK(v == csharp_vectors::tri_head[i]);
        if (i >= 2048 && i < 2080) CHECK(v == csharp_vectors::tri_after_wrap[i - 2048]);
        if (i < 2048) first.add(static_cast<std::uint32_t>(v));
        all.add(static_cast<std::uint32_t>(v));
    }
    CHECK(first.h == csharp_vectors::tri_fnv2048);
    CHECK(all.h == csharp_vectors::tri_fnv4200);

    // reset() restarts the sequence (interleaver boundary).
    t.next();
    t.reset();
    CHECK(t.next() == 5);
}

TEST_CASE("interleaver: D.5.3.3.2 worked example", "[interleaver][standard]") {
    // WID 1, 3 kHz, ultra-short: size 192, increment 25.
    const Interleaver il(192, 25);
    const std::uint32_t expect[9] = {0, 25, 50, 75, 100, 125, 150, 175, 8};
    for (std::uint32_t n = 0; n < 9; ++n) CHECK(il.load_location(n) == expect[n]);

    std::vector<std::uint8_t> in(192, 0), out(192, 0);
    for (std::uint32_t n = 0; n < 9; ++n) in[n] = static_cast<std::uint8_t>(n + 1);
    il.interleave(in.data(), out.data());
    for (std::uint32_t n = 0; n < 9; ++n) CHECK(out[expect[n]] == n + 1);
}

TEST_CASE("interleaver: every length and mode matches the C# reference and round trips", "[interleaver]") {
    for (const auto& v : csharp_vectors::interleavers) {
        CAPTURE(v.wn, v.kind, v.size, v.increment);
        const Interleaver il(v.size, v.increment);
        REQUIRE(il.valid());

        // Load locations: first 16 and a hash of all of them.
        Fnv lf;
        for (std::uint32_t n = 0; n < v.size; ++n) {
            if (n < 16) CHECK(il.load_location(n) == v.load16[n]);
            lf.add(il.load_location(n));
        }
        CHECK(lf.h == v.load_fnv);

        // Bytes through interleave(): same fetched array as the C#.
        std::vector<std::uint8_t> in(v.size), wire(v.size), back(v.size);
        for (std::uint32_t i = 0; i < v.size; ++i) in[i] = static_cast<std::uint8_t>(i & 255u);
        il.interleave(in.data(), wire.data());
        Fnv bf;
        for (std::uint8_t b : wire) bf.add(b);
        CHECK(bf.h == v.bytes_fnv);
        il.deinterleave(wire.data(), back.data());
        CHECK(back == in);

        // Soft values (float and double) round trip, and the permutation is a bijection.
        std::vector<float> fin(v.size), fwire(v.size), fback(v.size);
        for (std::uint32_t i = 0; i < v.size; ++i) fin[i] = static_cast<float>(i);
        il.interleave(fin.data(), fwire.data());
        std::vector<bool> seen(v.size, false);
        for (float f : fwire) seen[static_cast<std::size_t>(f)] = true;
        bool all_seen = true;
        for (bool b : seen) all_seen = all_seen && b;
        CHECK(all_seen);
        il.deinterleave(fwire.data(), fback.data());
        CHECK(fback == fin);
    }
}

TEST_CASE("interleaver: no allocation", "[interleaver][alloc]") {
    const Interleaver il(768, 97);
    std::vector<std::uint8_t> in(768, 1), out(768, 0);
    const ms110d_test::AllocationScope scope;
    il.interleave(in.data(), out.data());
    il.deinterleave(out.data(), in.data());
    Trinomial159Scrambler t;
    Scrambler s;
    int x = 0;
    for (int i = 0; i < 100; ++i) x += t.next() + s.next_psk(i & 7);
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(x > 0);
}
