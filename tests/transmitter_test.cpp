// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The public transmit path (issue 69): payload bits to 9600 Hz audio through
// m0lte::ms110d::Transmitter, against the C# reference audio (file 13) for every case of the
// standard set, with no reference file in between. Also chunking, lengths, state and
// argument checks, reuse, and that nothing allocates after configure().

#include <m0lte/ms110d/transmitter.hpp>

#include "alloc_guard.hpp"
#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <random>
#include <utility>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
using ms110d_test::Manifest;
using ms110d_test::ReferenceCase;

namespace {

namespace fs = std::filesystem;

constexpr double tolerance = 1e-6;  // the documented per-sample audio tolerance (docs/dev/modulator.md)

InterleaverKind kind_from_name(const std::string& s) {
    if (s == "ultrashort") return InterleaverKind::UltraShort;
    if (s == "short") return InterleaverKind::Short;
    if (s == "medium") return InterleaverKind::Medium;
    return InterleaverKind::Long;
}

TxSettings settings_of(const Manifest& m) {
    TxSettings c;
    c.waveform_number = static_cast<int>(m.integer("wn"));
    c.interleaver = kind_from_name(m.str("interleaver"));
    c.constraint_length = static_cast<int>(m.integer("constraint_length"));
    c.tlc_blocks = static_cast<int>(m.integer("tlc_blocks"));
    c.preamble_superframes = static_cast<int>(m.integer("preamble_superframes"));
    c.append_eom = m.integer("append_eom") != 0;
    c.append_eot = m.integer("append_eot") != 0;
    c.amplitude = static_cast<float>(m.real("amplitude"));
    c.roll_off = m.real("roll_off");
    return c;
}

// A transmitter and the memory it lives in.
struct Rig {
    std::vector<std::uint8_t> memory;
    Transmitter tx;

    Rig(const TxSettings& s, std::size_t max_bits) {
        std::size_t bytes = 0;
        REQUIRE(Transmitter::storage_bytes(s, max_bits, bytes) == Status::Ok);
        memory.resize(bytes);
        REQUIRE(tx.configure(s, max_bits, memory) == Status::Ok);
    }
};

// Runs one burst: all the payload in `push` bit pieces, then the audio in `chunk` sample reads.
std::vector<float> transmit(Transmitter& tx, const std::vector<std::uint8_t>& payload, std::size_t chunk,
                            std::size_t push = 0) {
    REQUIRE(tx.begin() == Status::Ok);
    if (push == 0) push = std::max<std::size_t>(payload.size(), 1);
    for (std::size_t at = 0; at < payload.size(); at += push) {
        const std::size_t n = std::min(push, payload.size() - at);
        REQUIRE(tx.push_bits(std::span<const std::uint8_t>(payload).subspan(at, n)) == Status::Ok);
    }
    REQUIRE(tx.end_payload() == Status::Ok);
    std::size_t total = 0;
    REQUIRE(tx.audio_samples(payload.size(), total) == Status::Ok);
    CHECK(tx.remaining() == total);
    std::vector<float> audio;
    audio.reserve(total);
    std::vector<float> buf(chunk);
    while (!tx.done()) {
        std::size_t got = 0;
        REQUIRE(tx.read(buf, got) == Status::Ok);
        REQUIRE(got > 0);
        audio.insert(audio.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    CHECK(audio.size() == total);
    CHECK(tx.remaining() == 0);
    return audio;
}

double max_error(const std::vector<float>& a, const std::vector<float>& ref) {
    REQUIRE(a.size() == ref.size());
    double err = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        err = std::max(err, std::fabs(static_cast<double>(a[i]) - static_cast<double>(ref[i])));
    }
    return err;
}

}  // namespace

TEST_CASE("reference end to end: payload bits to audio matches all 122 cases", "[reference][transmitter]") {
    // The C# audio, which has the standard probe pattern the C++ sends (issue 68).
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    const auto index = ms110d_test::load_index(dir);
    const std::size_t chunks[] = {1, 7, 64, 1000, 4096, 100000};
    std::size_t cases = 0;
    double worst = 0;
    for (const auto& name : index.cases) {
        INFO("case " << name);
        const ReferenceCase c(dir / name, name);
        const TxSettings s = settings_of(c.manifest());
        const auto payload = c.bytes("payload_bits");
        Rig rig(s, payload.size());
        const auto ref = c.real32("audio_f32le");
        // Chunk sizes rotate through the set, and the payload arrives in two pieces on odd cases.
        const std::size_t chunk = chunks[cases % std::size(chunks)];
        const auto audio = transmit(rig.tx, payload, chunk, cases % 2 == 1 ? payload.size() / 2 + 1 : 0);
        const double err = max_error(audio, ref);
        // The one-shot path gives the very same audio, and says what it needs when short.
        std::vector<float> shot(ref.size());
        std::size_t written = 0;
        REQUIRE(rig.tx.modulate(payload, shot, written) == Status::Ok);
        CHECK(written == ref.size());
        CHECK(shot == audio);
        std::vector<float> shorter(ref.size() - 1, 7.0f);
        CHECK(rig.tx.modulate(payload, shorter, written) == Status::BufferTooSmall);
        CHECK(written == ref.size());
        CHECK(std::all_of(shorter.begin(), shorter.end(), [](float x) { return x == 7.0f; }));
        worst = std::max(worst, err);
        CHECK(err <= tolerance);
        ++cases;
    }
    CHECK(cases == 122);
    INFO("worst error " << worst);
}

TEST_CASE("the chunk size never changes the audio", "[transmitter]") {
    for (const int wn : {0, 1, 6, 8, 13}) {
        INFO("wn " << wn);
        TxSettings s;
        s.waveform_number = wn;
        s.preamble_superframes = 1;
        std::mt19937 rng(7u + static_cast<unsigned>(wn));
        std::vector<std::uint8_t> payload(200);
        for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 1u);
        Rig rig(s, payload.size());
        const auto whole = transmit(rig.tx, payload, 1u << 20);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{31}, std::size_t{33}, std::size_t{999}}) {
            INFO("chunk " << chunk);
            const auto audio = transmit(rig.tx, payload, chunk, chunk);
            REQUIRE(audio.size() == whole.size());
            CHECK(std::equal(audio.begin(), audio.end(), whole.begin()));
        }
    }
}

TEST_CASE("audio_samples is the length the burst really has, for any payload, in every mode", "[transmitter]") {
    std::mt19937 rng(99);
    for (const Mode& mode : modes_3k()) {
        for (const InterleaverKind kind : {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium}) {
            if (!has_interleaver_3k(mode.wn, kind)) continue;
            INFO("wn " << mode.wn << " interleaver " << static_cast<int>(kind));
            TxSettings s;
            s.waveform_number = mode.wn;
            s.interleaver = kind;
            s.preamble_superframes = 1;
            const auto il = *interleaver_3k(mode.wn, kind);
            // Sizes around a block edge, where the EOM may or may not need another block.
            const std::size_t input = static_cast<std::size_t>(il.input_bits);
            std::vector<std::size_t> sizes = {0, 1, input, input + 1};
            if (input > 33) sizes.insert(sizes.end(), {input - 32 - 1, input - 32, input - 31});
            std::size_t biggest = 0;
            for (const std::size_t n : sizes) biggest = std::max(biggest, n);
            Rig rig(s, biggest);
            for (const std::size_t n : sizes) {
                std::vector<std::uint8_t> payload(n);
                for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 1u);
                std::size_t expected = 0;
                REQUIRE(rig.tx.audio_samples(n, expected) == Status::Ok);
                const auto audio = transmit(rig.tx, payload, 5000);
                CHECK(audio.size() == expected);
            }
        }
    }
}

TEST_CASE("the largest payload keeps sample counts inside 32 bits, in every mode", "[transmitter]") {
    for (const Mode& mode : modes_3k()) {
        for (const InterleaverKind kind : {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium, InterleaverKind::Long}) {
            if (!has_interleaver_3k(mode.wn, kind)) continue;
            TxSettings s;
            s.waveform_number = mode.wn;
            s.interleaver = kind;
            s.tlc_blocks = 255;
            s.preamble_superframes = 32;
            std::size_t bytes = 0;
            REQUIRE(Transmitter::storage_bytes(s, 0, bytes) == Status::Ok);
            std::vector<std::uint8_t> memory(bytes);
            Transmitter tx;
            REQUIRE(tx.configure(s, 0, memory) == Status::Ok);
            std::size_t samples = 0;
            REQUIRE(tx.audio_samples(max_payload_bits_limit, samples) == Status::Ok);
            CHECK(samples < std::numeric_limits<std::uint32_t>::max());
            CHECK(tx.audio_samples(max_payload_bits_limit + 1, samples) == Status::InvalidArgument);
        }
    }
}

TEST_CASE("a transmitter can be used for burst after burst", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    std::mt19937 rng(3);
    std::vector<std::uint8_t> a(4000), b(41);
    for (auto& x : a) x = static_cast<std::uint8_t>(rng() & 1u);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng() & 1u);
    Rig rig(s, a.size());
    const auto first = transmit(rig.tx, a, 100);
    const auto other = transmit(rig.tx, b, 100);
    CHECK(other.size() < first.size());
    CHECK(transmit(rig.tx, a, 77) == first);
    // Stopping part way and starting again is clean too.
    REQUIRE(rig.tx.begin() == Status::Ok);
    REQUIRE(rig.tx.push_bits(b) == Status::Ok);
    REQUIRE(rig.tx.end_payload() == Status::Ok);
    std::vector<float> few(10);
    std::size_t got = 0;
    REQUIRE(rig.tx.read(few, got) == Status::Ok);
    CHECK(got == 10);
    CHECK(transmit(rig.tx, a, 77) == first);
}

TEST_CASE("bits use only their low bit", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    std::vector<std::uint8_t> clean{1, 0, 1, 1, 0, 0, 1, 0, 1, 1};
    std::vector<std::uint8_t> dirty;
    for (const auto x : clean) dirty.push_back(static_cast<std::uint8_t>(x | 0xFEu));
    Rig rig(s, clean.size());
    CHECK(transmit(rig.tx, dirty, 500) == transmit(rig.tx, clean, 500));
}

TEST_CASE("settings are validated and the status says which kind of fault", "[transmitter]") {
    std::size_t bytes = 0;
    const auto sized = [&](const TxSettings& s, std::size_t max_bits = 100) { return Transmitter::storage_bytes(s, max_bits, bytes); };
    CHECK(sized(TxSettings{}) == Status::Ok);

    TxSettings s;
    s.waveform_number = 9;  // not implemented
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.waveform_number = -1;
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.constraint_length = 8;
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.preamble_superframes = 0;
    CHECK(sized(s) == Status::InvalidArgument);
    s.preamble_superframes = 33;
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.tlc_blocks = 256;
    CHECK(sized(s) == Status::InvalidArgument);
    s.tlc_blocks = -1;
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.roll_off = 0.04;
    CHECK(sized(s) == Status::InvalidArgument);
    s.roll_off = 1.01;
    CHECK(sized(s) == Status::InvalidArgument);
    s.roll_off = std::nan("");
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.amplitude = std::numeric_limits<float>::infinity();
    CHECK(sized(s) == Status::InvalidArgument);
    s.amplitude = std::nanf("");
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.interleaver = static_cast<InterleaverKind>(7);
    CHECK(sized(s) == Status::InvalidArgument);
    s = {};
    s.waveform_number = 0;
    s.interleaver = InterleaverKind::UltraShort;  // no such row in Table D-XXXVII
    CHECK(sized(s) == Status::Unsupported);
    CHECK(sized(TxSettings{}, max_payload_bits_limit + 1) == Status::InvalidArgument);
    CHECK(sized(TxSettings{}, max_payload_bits_limit) == Status::Ok);

    // Every defined pair is accepted, with both codes.
    for (const Mode& m : modes_3k()) {
        for (const InterleaverKind kind : {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium, InterleaverKind::Long}) {
            for (const int k : {7, 9}) {
                TxSettings t;
                t.waveform_number = m.wn;
                t.interleaver = kind;
                t.constraint_length = k;
                CHECK(sized(t) == (has_interleaver_3k(m.wn, kind) ? Status::Ok : Status::Unsupported));
            }
        }
    }
}

TEST_CASE("configure checks the storage and leaves a failed object unconfigured", "[transmitter]") {
    const TxSettings s;
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, 64, bytes) == Status::Ok);
    std::vector<std::uint8_t> memory(bytes);
    Transmitter tx;
    CHECK_FALSE(tx.configured());
    CHECK(tx.configure(s, 64, std::span<std::uint8_t>(memory).first(bytes - 64)) == Status::BufferTooSmall);
    CHECK_FALSE(tx.configured());
    CHECK(tx.configure(s, 64, {}) == Status::BufferTooSmall);
    TxSettings bad;
    bad.constraint_length = 5;
    CHECK(tx.configure(bad, 64, memory) == Status::InvalidArgument);
    CHECK_FALSE(tx.configured());
    // Everything before configure is a state error, and nothing is written.
    std::size_t out = 123;
    CHECK(tx.begin() == Status::State);
    CHECK(tx.push_bits({}) == Status::State);
    CHECK(tx.end_payload() == Status::State);
    CHECK(tx.audio_samples(10, out) == Status::State);
    float one_sample[1];
    CHECK(tx.read(one_sample, out) == Status::State);
    CHECK(tx.modulate({}, one_sample, out) == Status::State);
    CHECK(out == 0);
    CHECK(tx.remaining() == 0);
    CHECK_FALSE(tx.done());

    // Storage at an odd address works too (it is aligned inside).
    std::vector<std::uint8_t> padded(bytes + 1);
    CHECK(tx.configure(s, 64, std::span<std::uint8_t>(padded).subspan(1)) == Status::Ok);
    CHECK(tx.configured());
}

TEST_CASE("the steps must come in order and the payload must fit", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    Rig rig(s, 10);
    Transmitter& tx = rig.tx;
    std::vector<float> buf(16);
    std::size_t got = 99;
    // Reading before the end of the payload.
    CHECK(tx.read(buf, got) == Status::State);
    CHECK(got == 0);
    CHECK(tx.remaining() == 0);
    CHECK_FALSE(tx.done());
    // Too many bits: none are taken, and what fits still can be.
    const std::vector<std::uint8_t> bits(6, 1);
    CHECK(tx.push_bits(bits) == Status::Ok);
    CHECK(tx.push_bits(bits) == Status::BufferTooSmall);
    CHECK(tx.push_bits(std::span<const std::uint8_t>(bits).first(4)) == Status::Ok);
    CHECK(tx.push_bits(std::span<const std::uint8_t>(bits).first(1)) == Status::BufferTooSmall);
    CHECK(tx.push_bits({}) == Status::Ok);
    CHECK(tx.end_payload() == Status::Ok);
    CHECK(tx.end_payload() == Status::State);
    CHECK(tx.push_bits({}) == Status::State);
    // The 10 bits sent are the whole payload.
    std::size_t expected = 0;
    REQUIRE(tx.audio_samples(10, expected) == Status::Ok);
    CHECK(tx.remaining() == expected);
    // An empty read is an error, so that zero samples only ever means the burst is finished.
    CHECK(tx.read(std::span<float>{}, got) == Status::InvalidArgument);
    CHECK(got == 0);
    CHECK(tx.remaining() == expected);
    // Reading past the end gives zero samples, not an error.
    std::vector<float> all(expected + 100);
    CHECK(tx.read(all, got) == Status::Ok);
    CHECK(got == expected);
    CHECK(tx.done());
    CHECK(tx.read(all, got) == Status::Ok);
    CHECK(got == 0);
    // begin() makes it usable again, and a payload may be empty.
    REQUIRE(tx.begin() == Status::Ok);
    CHECK_FALSE(tx.done());
    CHECK(tx.end_payload() == Status::Ok);
    REQUIRE(tx.audio_samples(0, expected) == Status::Ok);
    CHECK(tx.remaining() == expected);
}

TEST_CASE("transmitting does not allocate once configured", "[transmitter][allocation]") {
    // The heaviest cases: 16QAM with the long interleaver (a 49152 bit block), WN 0 and the
    // K = 9 code, and a roll-off with a long pulse.
    struct Pick {
        int wn;
        InterleaverKind kind;
        int k;
        double roll_off;
    };
    for (const Pick p : {Pick{8, InterleaverKind::Long, 7, 0.35}, Pick{0, InterleaverKind::Long, 9, 0.35},
                         Pick{6, InterleaverKind::Short, 9, 0.05}, Pick{1, InterleaverKind::UltraShort, 7, 1.0}}) {
        INFO("wn " << p.wn);
        TxSettings s;
        s.waveform_number = p.wn;
        s.interleaver = p.kind;
        s.constraint_length = p.k;
        s.roll_off = p.roll_off;
        s.preamble_superframes = 1;
        std::vector<std::uint8_t> payload(500);
        std::mt19937 rng(11);
        for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 1u);
        Rig rig(s, payload.size());
        std::vector<float> buf(777);
        std::size_t total = 0;
        REQUIRE(rig.tx.audio_samples(payload.size(), total) == Status::Ok);

        std::size_t read_total = 0, calls = 0;
        bool ok = true;
        const ms110d_test::AllocationScope scope;
        for (int burst = 0; burst < 2; ++burst) {
            ok = ok && rig.tx.begin() == Status::Ok;
            ok = ok && rig.tx.push_bits(payload) == Status::Ok;
            ok = ok && rig.tx.end_payload() == Status::Ok;
            std::size_t n = 0;
            ok = ok && rig.tx.audio_samples(payload.size(), n) == Status::Ok;
            while (!rig.tx.done()) {
                std::size_t got = 0;
                ok = ok && rig.tx.read(buf, got) == Status::Ok;
                read_total += got;
                ++calls;
            }
        }
        const std::size_t seen = scope.allocations();
        CHECK(ok);
        CHECK(read_total == 2 * total);
        CHECK(calls > 2);
        CHECK(seen == 0);
    }
}

TEST_CASE("modulate asks for the length with an empty buffer, and refuses too long a payload", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    Rig rig(s, 20);
    const std::vector<std::uint8_t> bits(20, 1);
    std::size_t need = 0;
    REQUIRE(rig.tx.audio_samples(20, need) == Status::Ok);
    std::size_t written = 99;
    CHECK(rig.tx.modulate(bits, std::span<float>{}, written) == Status::BufferTooSmall);
    CHECK(written == need);
    const std::vector<std::uint8_t> too_many(21, 1);
    CHECK(rig.tx.modulate(too_many, std::span<float>{}, written) == Status::BufferTooSmall);
    CHECK(written == 0);
    // A size query does not disturb a burst being streamed.
    std::vector<float> part(10), rest(need);
    REQUIRE(rig.tx.begin() == Status::Ok);
    REQUIRE(rig.tx.push_bits(bits) == Status::Ok);
    REQUIRE(rig.tx.end_payload() == Status::Ok);
    REQUIRE(rig.tx.read(part, written) == Status::Ok);
    CHECK(rig.tx.modulate(bits, std::span<float>{}, written) == Status::BufferTooSmall);
    CHECK(rig.tx.remaining() == need - 10);
    // A failed call leaves the transmitter usable.
    std::vector<float> audio(need);
    CHECK(rig.tx.modulate(bits, audio, written) == Status::Ok);
    CHECK(written == need);
    CHECK(rig.tx.done());
}

TEST_CASE("a transmitter can be moved, and the old one is left unconfigured", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    Rig rig(s, 20);
    const std::vector<std::uint8_t> bits(20, 1);
    const auto before = transmit(rig.tx, bits, 100);
    Transmitter moved(std::move(rig.tx));
    CHECK_FALSE(rig.tx.configured());
    CHECK(moved.configured());
    CHECK(transmit(moved, bits, 100) == before);
    Transmitter assigned;
    assigned = std::move(moved);
    CHECK_FALSE(moved.configured());
    Transmitter& alias = assigned;
    assigned = std::move(alias);
    CHECK(assigned.configured());
    CHECK(transmit(assigned, bits, 100) == before);
}

TEST_CASE("a failed configure leaves storage that is in use untouched", "[transmitter]") {
    TxSettings s;
    s.preamble_superframes = 1;
    Rig rig(s, 20);
    const std::vector<std::uint8_t> bits(20, 1);
    const auto before = transmit(rig.tx, bits, 100);
    const auto snapshot = rig.memory;
    TxSettings bad = s;
    bad.roll_off = 5.0;
    CHECK(rig.tx.configure(bad, 20, rig.memory) == Status::InvalidArgument);
    CHECK(rig.tx.configure(s, 20, std::span<std::uint8_t>(rig.memory).first(10)) == Status::BufferTooSmall);
    CHECK(rig.memory == snapshot);
    CHECK(transmit(rig.tx, bits, 100) == before);
}
