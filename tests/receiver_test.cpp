// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The public receiver (include/m0lte/ms110d/receiver.hpp), end to end: every reference case
// through it against the C# 0.4.1 receiver (bit for bit in the default build) and against the
// internal path it is built on (bit for bit in both builds); chunk size independence; zero
// allocation; several bursts in one stream; the options; and misuse. docs/dev/receiver.md.

#include <m0lte/ms110d/receiver.hpp>
#include <m0lte/ms110d/transmitter.hpp>

#include "alloc_guard.hpp"
#include "reference_files.hpp"
#include "rx_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <tuple>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace m0lte::ms110d;
namespace fs = std::filesystem;

namespace {

#ifdef MS110D_SINGLE_PRECISION
constexpr bool strict = false;
#else
constexpr bool strict = true;
#endif

struct Fnv {
    std::uint64_t h = 14695981039346656037ULL;
    void add(std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 1099511628211ULL;
        }
    }
    void add(float f) { add(std::bit_cast<std::uint32_t>(f)); }
};

std::string hex(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

template <class C>
std::string bit_hash(const C& bits) {
    Fnv f;
    for (const auto b : bits) f.add(static_cast<std::uint32_t>(b));
    return std::to_string(bits.size()) + " " + hex(f.h);
}

std::string float_hash(const std::vector<float>& v) {
    Fnv f;
    for (const float x : v) f.add(x);
    return hex(f.h);
}

// Everything a receiver run raised, copied out of the events.
struct Ev {
    RxEventKind kind = RxEventKind::None;
    std::int64_t sample = 0;
    int waveform = -1, k = 0, block = -1, blocks = 0;
    InterleaverKind il = InterleaverKind::Short;
    double cfo = 0;
    std::vector<std::uint8_t> bits;
    std::vector<float> soft;
    BurstEndReason reason = BurstEndReason::Eom;
    std::int64_t payload_bits = 0;
    bool operator==(const Ev&) const = default;
};

std::vector<Ev> drain(Receiver& rx) {
    std::vector<Ev> out;
    for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
        Ev v;
        v.kind = e.kind;
        v.sample = e.sample;
        v.waveform = e.waveform;
        v.k = e.constraint_length;
        v.il = e.interleaver;
        v.cfo = e.cfo_hz;
        v.block = e.block_index;
        v.bits.assign(e.bits.begin(), e.bits.end());
        v.soft.assign(e.soft.begin(), e.soft.end());
        v.reason = e.reason;
        v.blocks = e.blocks;
        v.payload_bits = e.payload_bits;
        out.push_back(std::move(v));
    }
    return out;
}

// Feeds audio in chunks (the sizes cycle through `chunks`) and collects every event.
std::vector<Ev> run(Receiver& rx, const std::vector<float>& audio, const std::vector<std::size_t>& chunks = {2400}) {
    std::vector<Ev> out;
    std::size_t at = 0;
    std::size_t c = 0;
    while (at < audio.size()) {
        const std::size_t n = std::min(chunks[c++ % chunks.size()], audio.size() - at);
        std::size_t done = 0;
        while (done < n) {
            std::size_t used = 0;
            REQUIRE(rx.process(std::span<const float>(audio.data() + at + done, n - done), used) == Status::Ok);
            done += used;
            auto ev = drain(rx);
            out.insert(out.end(), ev.begin(), ev.end());
        }
        at += n;
    }
    return out;
}

struct Made {
    std::vector<std::uint8_t> memory;
    std::unique_ptr<Receiver> rx;
};

Made make(const RxOptions& o) {
    Made m;
    std::size_t bytes = 0;
    REQUIRE(Receiver::storage_bytes(o, bytes) == Status::Ok);
    m.memory.resize(bytes);
    m.rx = std::make_unique<Receiver>();
    REQUIRE(m.rx->configure(o, m.memory) == Status::Ok);
    return m;
}

// The payload of the first burst: its block bits cut to payload_bits.
std::vector<std::uint8_t> payload_of(const std::vector<Ev>& ev, std::size_t burst = 0) {
    std::vector<std::uint8_t> bits;
    std::size_t seen = 0;
    for (const Ev& e : ev) {
        if (e.kind == RxEventKind::BlockDecoded && seen == burst) bits.insert(bits.end(), e.bits.begin(), e.bits.end());
        if (e.kind == RxEventKind::BurstCompleted) {
            if (seen == burst) {
                bits.resize(static_cast<std::size_t>(e.payload_bits));
                return bits;
            }
            ++seen;
        }
    }
    return {};
}

const Ev* find(const std::vector<Ev>& ev, RxEventKind kind, std::size_t nth = 0) {
    for (const Ev& e : ev) {
        if (e.kind == kind && nth-- == 0) return &e;
    }
    return nullptr;
}

std::size_t count(const std::vector<Ev>& ev, RxEventKind kind) {
    return static_cast<std::size_t>(std::count_if(ev.begin(), ev.end(), [kind](const Ev& e) { return e.kind == kind; }));
}

std::string il_name(InterleaverKind k) {
    switch (k) {
        case InterleaverKind::UltraShort: return "ultrashort";
        case InterleaverKind::Short: return "short";
        case InterleaverKind::Medium: return "medium";
        case InterleaverKind::Long: return "long";
    }
    return "?";
}

const char* reason_name(BurstEndReason r) {
    switch (r) {
        case BurstEndReason::Eom: return "Eom";
        case BurstEndReason::MaxInputDataBlocks: return "MaxInputDataBlocks";
        case BurstEndReason::Terminated: return "Terminated";
        case BurstEndReason::SignalLost: return "SignalLost";
        case BurstEndReason::SignalAbsent: return "SignalAbsent";
        case BurstEndReason::NewPreamble: return "NewPreamble";
    }
    return "?";
}

// The C# rx-turbo dump, as much of it as the public events can be held to.
struct CsBlock {
    long long at = 0;
    std::string first_hash, info;
    bool turbo = false;
};
struct CsCase {
    std::string label;
    int wn = -1;
    std::vector<CsBlock> blocks;
    std::string reason, payload_hash;
    int end_blocks = 0;
    long long payload_bits = 0, end_at = 0;
};

std::map<std::string, CsCase> load_turbo(const char* path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::map<std::string, CsCase> out;
    CsCase* cur = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "case") {
            CsCase c;
            std::string w;
            s >> c.label >> w >> c.wn;
            cur = &(out[c.label] = c);
        } else if (kind == "wn") {
            std::string w;
            s >> cur->wn;
        } else if (kind == "block") {
            CsBlock b;
            int index = 0;
            s >> index >> b.at;
            cur->blocks.push_back(b);
        } else if (kind == "first") {
            s >> cur->blocks.back().first_hash;
        } else if (kind == "turbo") {
            std::string h;
            s >> h;
            cur->blocks.back().turbo = h != "-";
        } else if (kind == "info") {
            std::string n, h;
            s >> n >> h;
            cur->blocks.back().info = n + " " + h;
        } else if (kind == "end") {
            std::string n, h;
            s >> cur->reason >> cur->end_blocks >> cur->payload_bits >> cur->end_at >> n >> h;
            cur->payload_hash = n + " " + h;
        }
    }
    return out;
}

// The C# WN 0 dump: per case the decoded bits and the block count.
struct CsWn0 {
    int blocks = 0;
    std::vector<std::uint8_t> info;
    double chip0 = 0;
};

std::map<std::string, CsWn0> load_wn0(const char* path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::map<std::string, CsWn0> out;
    std::string line, label;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "case") {
            std::string w;
            int locked = 0;
            s >> label >> w >> locked >> w >> out[label].blocks;
        } else if (kind == "info") {
            std::string bits;
            s >> bits;
            for (const char c : bits) out[label].info.push_back(c == '1' ? 1 : 0);
        } else if (kind == "start") {
            std::string w;
            s >> w >> out[label].chip0;
        }
    }
    return out;
}

void require_file(const std::string& path) {
    if (std::ifstream(path).good()) return;
    if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("missing: " << path);
    SKIP("no file at " << path << " (run scripts/generate-rx-dumps.sh)");
}

std::vector<float> case_audio(const ms110d_test::ReferenceCase& rc) {
    std::vector<float> audio = rc.real32("audio_f32le");
    audio.resize(audio.size() + 9600, 0.0f);  // as the C# dumps: 9600 zero samples after
    return audio;
}

}  // namespace

TEST_CASE("reference receiver: every reference case through the public API, against the C# and the internal path",
          "[receiver]") {
    const fs::path dir = ms110d_test::reference_dir_or_skip();
    require_file(MS110D_CSHARP_RX_TURBO);
    const auto turbo = load_turbo(MS110D_CSHARP_RX_TURBO);
    const auto wn0 = load_wn0(MS110D_CSHARP_WN0_DUMP);
    const ms110d_test::Index index = ms110d_test::load_index(dir);
    REQUIRE(index.cases.size() == 122);

    RxOptions o;
    o.soft_decisions = true;
    Made m = make(o);
    ms110d_test::RxDriver driver(InterleaverKind::Long, false);
    // The chunk sizes rotate from case to case.
    const std::vector<std::vector<std::size_t>> chunkings = {{2400}, {1, 7, 333, 9600}, {96}, {4801, 2}, {65536}};
    long cases = 0, wn0_cases = 0, blocks = 0, payload_right = 0, eom_cases = 0, soft_equal = 0, soft_compared = 0;
    for (const std::string& name : index.cases) {
        INFO("case " << name);
        const ms110d_test::ReferenceCase rc(dir / name, name);
        const std::vector<float> audio = case_audio(rc);
        const auto sent = rc.bytes("payload_bits");
        m.rx->reset();
        const std::vector<Ev> ev = run(*m.rx, audio, chunkings[static_cast<std::size_t>(cases) % chunkings.size()]);
        ++cases;
        const int wn = static_cast<int>(rc.manifest().integer("wn"));
        const Ev* started = find(ev, RxEventKind::BurstStarted);
        const Ev* end = find(ev, RxEventKind::BurstCompleted);

        if (wn == 0) {
            ++wn0_cases;
            const auto it = wn0.find(name);
            REQUIRE(it != wn0.end());
            REQUIRE(started != nullptr);
            CHECK(started->waveform == 0);
            REQUIRE(end != nullptr);
            CHECK(end->blocks == it->second.blocks);
            std::vector<std::uint8_t> all;
            for (const Ev& e : ev) {
                if (e.kind == RxEventKind::BlockDecoded) all.insert(all.end(), e.bits.begin(), e.bits.end());
            }
            CHECK(all == it->second.info);  // every decoded bit, as the C#
            CHECK(end->reason == BurstEndReason::Eom);
            CHECK(payload_of(ev) == sent);
            ++eom_cases;
            if (payload_of(ev) == sent) ++payload_right;
            blocks += end->blocks;
            continue;
        }

        const auto it = turbo.find(name);
        REQUIRE(it != turbo.end());
        const CsCase& c = it->second;
        const ms110d_test::RxRun r = driver.run(audio);
        if (c.wn < 0) {
            // A one super-frame preamble: neither the C# nor this acquires it.
            CHECK(ev.empty());
            continue;
        }
        REQUIRE(started != nullptr);
        CHECK(started->waveform == c.wn);
        CHECK(started->waveform == wn);
        CHECK(il_name(started->il) == rc.manifest().str("interleaver"));
        CHECK(started->k == static_cast<int>(rc.manifest().integer("constraint_length")));
        CHECK(started->sample == r.detection.locked_at_sample);

        // Against the internal path (both builds): the same blocks, soft values, samples and end.
        REQUIRE(count(ev, RxEventKind::BlockDecoded) == r.blocks.size());
        for (std::size_t i = 0; i < r.blocks.size(); ++i) {
            const Ev& b = *find(ev, RxEventKind::BlockDecoded, i);
            const Ev& f = *find(ev, RxEventKind::FirstPassSoft, i);
            CHECK(b.block == static_cast<int>(i));
            CHECK(b.bits == r.blocks[i].info);
            CHECK(b.sample == 2 * r.blocks[i].at_t2);
            CHECK(f.soft == r.blocks[i].llrs);
            CHECK(f.sample == b.sample);
            CHECK(b.waveform == c.wn);
        }
        CHECK(count(ev, RxEventKind::TurboSoft) ==
              static_cast<std::size_t>(std::count_if(r.blocks.begin(), r.blocks.end(), [](const auto& b) { return !b.turbo_llrs.empty(); })));

        // Against the C# (to the bit in the default build; the clean cases take the C#'s every
        // decision in the single build too, so the bits and the end are held exactly in both).
        REQUIRE(count(ev, RxEventKind::BlockDecoded) == c.blocks.size());
        for (std::size_t i = 0; i < c.blocks.size(); ++i) {
            const Ev& b = *find(ev, RxEventKind::BlockDecoded, i);
            CHECK(bit_hash(b.bits) == c.blocks[i].info);
            CHECK(b.sample == 2 * c.blocks[i].at);
            ++soft_compared;
            if (float_hash(find(ev, RxEventKind::FirstPassSoft, i)->soft) == c.blocks[i].first_hash) ++soft_equal;
            ++blocks;
        }
        if (c.reason == "None") {
            // No EOM and the audio ran out first: the burst has not ended, in either.
            CHECK(end == nullptr);
            continue;
        }
        REQUIRE(end != nullptr);
        CHECK(reason_name(end->reason) == c.reason);
        CHECK(end->blocks == c.end_blocks);
        CHECK(end->payload_bits == c.payload_bits);
        CHECK(end->sample == 2 * c.end_at);
        CHECK(bit_hash(payload_of(ev)) == c.payload_hash);
        if (c.reason == "Eom") {
            ++eom_cases;
            if (payload_of(ev) == sent) ++payload_right;
        }
    }
    std::printf("public receiver, %ld reference cases (%ld WN 0): %ld blocks, payload right on %ld of %ld EOM cases, "
                "first-pass soft values equal to the C# bits on %ld of %ld blocks\n",
                cases, wn0_cases, blocks, payload_right, eom_cases, soft_equal, soft_compared);
    CHECK(payload_right == eom_cases);
    if (strict) CHECK(soft_equal == soft_compared);
}

TEST_CASE("receiver: chunk size never changes the events", "[receiver]") {
    // Transmitter audio of a few modes, so this runs without the reference files.
    auto audio_for = [](int wn, InterleaverKind il, std::size_t bits) {
        TxSettings s;
        s.waveform_number = wn;
        s.interleaver = il;
        std::size_t bytes = 0;
        REQUIRE(Transmitter::storage_bytes(s, bits, bytes) == Status::Ok);
        std::vector<std::uint8_t> mem(bytes);
        Transmitter tx;
        REQUIRE(tx.configure(s, bits, mem) == Status::Ok);
        std::vector<std::uint8_t> payload(bits);
        for (std::size_t i = 0; i < bits; ++i) payload[i] = static_cast<std::uint8_t>((i * 2654435761u >> 7) & 1);
        std::size_t n = 0;
        REQUIRE(tx.audio_samples(bits, n) == Status::Ok);
        std::vector<float> audio(3000 + n + 9600, 0.0f);
        std::size_t written = 0;
        REQUIRE(tx.modulate(payload, std::span<float>(audio.data() + 3000, n), written) == Status::Ok);
        return std::make_pair(audio, payload);
    };
    RxOptions o;
    o.soft_decisions = true;
    o.max_interleaver = InterleaverKind::Short;
    Made m = make(o);
    for (const auto& [wn, il, bits] : {std::tuple{0, InterleaverKind::Short, 300}, std::tuple{2, InterleaverKind::Short, 500},
                                       std::tuple{7, InterleaverKind::UltraShort, 2000}}) {
        INFO("WN " << wn);
        const auto [audio, payload] = audio_for(wn, il, static_cast<std::size_t>(bits));
        m.rx->reset();
        const std::vector<Ev> base = run(*m.rx, audio, {audio.size()});
        REQUIRE(payload_of(base) == payload);
        for (const std::vector<std::size_t>& chunks :
             std::vector<std::vector<std::size_t>>{{1}, {2}, {3}, {7}, {64}, {577}, {1000}, {4800}, {9600}, {1, 9600, 13, 2400}}) {
            m.rx->reset();
            CHECK(run(*m.rx, audio, chunks) == base);
        }
        // Every chunk size from 1 to 9600 in turn, through the stream.
        std::vector<std::size_t> all(9600);
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = i + 1;
        m.rx->reset();
        CHECK(run(*m.rx, audio, all) == base);
    }
}

TEST_CASE("receiver: nothing allocates after configure", "[receiver]") {
    TxSettings s;
    s.waveform_number = 7;
    s.interleaver = InterleaverKind::Short;
    const std::size_t bits = 3000;
    std::size_t bytes = 0;
    REQUIRE(Transmitter::storage_bytes(s, bits, bytes) == Status::Ok);
    std::vector<std::uint8_t> mem(bytes);
    Transmitter tx;
    REQUIRE(tx.configure(s, bits, mem) == Status::Ok);
    std::vector<std::uint8_t> payload(bits, 1);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(bits, n) == Status::Ok);
    std::vector<float> audio(n + 9600, 0.0f);
    std::size_t written = 0;
    REQUIRE(tx.modulate(payload, audio, written) == Status::Ok);
    s.waveform_number = 0;
    REQUIRE(Transmitter::storage_bytes(s, 400, bytes) == Status::Ok);
    mem.assign(bytes, 0);
    REQUIRE(tx.configure(s, 400, mem) == Status::Ok);
    REQUIRE(tx.audio_samples(400, n) == Status::Ok);
    const std::size_t first = audio.size();
    audio.resize(first + n + 9600, 0.0f);
    payload.assign(400, 0);
    REQUIRE(tx.modulate(payload, std::span<float>(audio.data() + first, n), written) == Status::Ok);

    RxOptions o;
    o.soft_decisions = true;
    o.max_interleaver = InterleaverKind::Short;
    Made m = make(o);
    std::size_t events = 0, completed = 0;
    std::uint64_t h = 0;
    const ms110d_test::AllocationScope scope;
    std::size_t at = 0;
    while (at < audio.size()) {
        std::size_t used = 0;
        m.rx->process(std::span<const float>(audio.data() + at, std::min<std::size_t>(1000, audio.size() - at)), used);
        at += used;
        for (RxEvent e = m.rx->next_event(); e.kind != RxEventKind::None; e = m.rx->next_event()) {
            ++events;
            if (e.kind == RxEventKind::BurstCompleted) ++completed;
            for (const auto b : e.bits) h = (h * 31) + b;
        }
    }
    m.rx->terminate();
    m.rx->reset();
    const std::size_t seen = scope.allocations();
    CHECK(seen == 0);
    CHECK(completed == 2);
    CHECK(events > 10);
    CHECK(h != 0);
}

TEST_CASE("receiver: bursts one after another in one stream, terminate, the block limit and refusals", "[receiver]") {
    auto burst = [](int wn, InterleaverKind il, std::size_t bits, std::uint8_t seed) {
        TxSettings s;
        s.waveform_number = wn;
        s.interleaver = il;
        std::size_t bytes = 0;
        REQUIRE(Transmitter::storage_bytes(s, bits, bytes) == Status::Ok);
        std::vector<std::uint8_t> mem(bytes);
        Transmitter tx;
        REQUIRE(tx.configure(s, bits, mem) == Status::Ok);
        std::vector<std::uint8_t> payload(bits);
        for (std::size_t i = 0; i < bits; ++i) payload[i] = static_cast<std::uint8_t>(((i + seed) * 40503u >> 5) & 1);
        std::size_t n = 0;
        REQUIRE(tx.audio_samples(bits, n) == Status::Ok);
        std::vector<float> audio(n);
        std::size_t written = 0;
        REQUIRE(tx.modulate(payload, audio, written) == Status::Ok);
        return std::make_pair(audio, payload);
    };
    const auto [a1, p1] = burst(3, InterleaverKind::Short, 900, 1);
    const auto [a2, p2] = burst(0, InterleaverKind::Short, 200, 2);
    const auto [a3, p3] = burst(6, InterleaverKind::UltraShort, 1500, 3);
    std::vector<float> stream(4000, 0.0f);
    for (const auto* a : {&a1, &a2, &a3}) {
        stream.insert(stream.end(), a->begin(), a->end());
        stream.insert(stream.end(), 12000, 0.0f);
    }

    SECTION("three bursts decode in turn, the receiver searching again after each") {
        Made m = make(RxOptions{});
        const std::vector<Ev> ev = run(*m.rx, stream);
        REQUIRE(count(ev, RxEventKind::BurstCompleted) == 3);
        CHECK(count(ev, RxEventKind::BurstStarted) == 3);
        CHECK(count(ev, RxEventKind::FirstPassSoft) == 0);  // soft decisions are off by default
        CHECK(payload_of(ev, 0) == p1);
        CHECK(payload_of(ev, 1) == p2);
        CHECK(payload_of(ev, 2) == p3);
        CHECK(find(ev, RxEventKind::BurstStarted, 1)->waveform == 0);
        CHECK(m.rx->state() == RxState::Searching);
        CHECK(m.rx->stats().turbo_skipped > 0);  // the WN 0 blocks
        // Positions increase and every event of a burst carries its lock.
        for (std::size_t i = 1; i < ev.size(); ++i) CHECK(ev[i].sample >= ev[i - 1].sample);
    }
    SECTION("the block limit and disabling turbo") {
        RxOptions o;
        o.max_input_data_blocks = 1;
        o.disable_turbo = true;
        o.soft_decisions = true;
        Made m = make(o);
        const std::vector<Ev> ev = run(*m.rx, stream);
        REQUIRE(count(ev, RxEventKind::BurstCompleted) == 3);
        CHECK(find(ev, RxEventKind::BurstCompleted)->reason == BurstEndReason::MaxInputDataBlocks);
        CHECK(find(ev, RxEventKind::BurstCompleted)->blocks == 1);
        CHECK(count(ev, RxEventKind::TurboSoft) == 0);
        CHECK(m.rx->stats().turbo_converged == 0);
    }
    SECTION("terminate ends the burst in progress and the search goes on") {
        Made m = make(RxOptions{});
        std::vector<Ev> ev;
        std::size_t at = 0;
        bool terminated = false;
        while (at < stream.size()) {
            std::size_t used = 0;
            REQUIRE(m.rx->process(std::span<const float>(stream.data() + at, std::min<std::size_t>(480, stream.size() - at)), used) ==
                    Status::Ok);
            at += used;
            auto got = drain(*m.rx);
            ev.insert(ev.end(), got.begin(), got.end());
            if (!terminated && count(ev, RxEventKind::BlockDecoded) == 1) {
                CHECK(m.rx->state() == RxState::Tracking);
                CHECK(m.rx->carrier_detect());
                m.rx->terminate();
                terminated = true;
            }
        }
        REQUIRE(count(ev, RxEventKind::BurstCompleted) == 3);
        const Ev* first = find(ev, RxEventKind::BurstCompleted);
        CHECK(first->reason == BurstEndReason::Terminated);
        CHECK(first->blocks == 1);
        CHECK(payload_of(ev, 1) == p2);
        CHECK(payload_of(ev, 2) == p3);
    }
    SECTION("a new preamble ends a burst that lost its EOM, and the new burst decodes (C# issue 10)") {
        // The first burst is cut half way, so its EOM never comes; the next starts a second later.
        // The background search finds its preamble and the receiver lets go of the dead burst.
        for (const int wn : {3, 0}) {
            INFO("WN " << wn);
            const auto [cut, pc] = burst(wn, InterleaverKind::Short, wn == 0 ? 600 : 4000, 6);
            const auto [next, pn] = burst(wn, InterleaverKind::Short, wn == 0 ? 200 : 800, 7);
            std::vector<float> s(3000, 0.0f);
            s.insert(s.end(), cut.begin(), cut.begin() + static_cast<std::ptrdiff_t>(cut.size() / 2));
            s.insert(s.end(), 9600, 0.0f);
            s.insert(s.end(), next.begin(), next.end());
            s.insert(s.end(), 9600, 0.0f);
            Made m = make(RxOptions{});
            const std::vector<Ev> ev = run(*m.rx, s);
            REQUIRE(count(ev, RxEventKind::BurstCompleted) == 2);
            CHECK(count(ev, RxEventKind::BurstStarted) == 2);
            CHECK(find(ev, RxEventKind::BurstCompleted, 0)->reason == BurstEndReason::NewPreamble);
            CHECK(find(ev, RxEventKind::BurstCompleted, 1)->reason == BurstEndReason::Eom);
            CHECK(payload_of(ev, 1) == pn);
            // Let go of in time to lock the new burst within 20 ms of the end of its three super-frame
            // preamble, as a receiver that was searching would (the filter delay and the vote span).
            CHECK(find(ev, RxEventKind::BurstCompleted, 0)->sample < find(ev, RxEventKind::BurstStarted, 1)->sample);
            CHECK(find(ev, RxEventKind::BurstStarted, 1)->sample <
                  static_cast<std::int64_t>(3000 + (cut.size() / 2) + 9600 + (3 * 576 * 4) + 192));
        }
    }
    SECTION("a new preamble the receiver would refuse does not end the burst it is decoding") {
        // The cut burst is followed by a long interleaver burst. Without a cap that preamble lets go
        // of the dead burst; under a short cap it would only be refused, so the burst is kept.
        const auto [cut, pc] = burst(3, InterleaverKind::Short, 4000, 8);
        const auto [lng, pl] = burst(3, InterleaverKind::Long, 200, 9);
        std::vector<float> s(3000, 0.0f);
        s.insert(s.end(), cut.begin(), cut.begin() + static_cast<std::ptrdiff_t>(cut.size() / 2));
        s.insert(s.end(), 9600, 0.0f);
        s.insert(s.end(), lng.begin(), lng.end());
        s.insert(s.end(), 9600, 0.0f);
        Made all = make(RxOptions{});
        const std::vector<Ev> ev_all = run(*all.rx, s);
        REQUIRE(count(ev_all, RxEventKind::BurstCompleted) == 2);
        CHECK(find(ev_all, RxEventKind::BurstCompleted, 0)->reason == BurstEndReason::NewPreamble);
        CHECK(payload_of(ev_all, 1) == pl);
        RxOptions o;
        o.max_interleaver = InterleaverKind::Short;
        Made capped = make(o);
        const std::vector<Ev> ev = run(*capped.rx, s);
        // The first burst is still held at the end of the audio: not let go of, and the long one not
        // received (it is never locked live, so no refusal is raised either).
        CHECK(count(ev, RxEventKind::BurstStarted) == 1);
        CHECK(count(ev, RxEventKind::BurstCompleted) == 0);
        CHECK(capped.rx->state() == RxState::Tracking);
    }
    SECTION("bursts with a longer interleaver than configured are refused with an event, the others decode") {
        const auto [l3, lp3] = burst(3, InterleaverKind::Long, 200, 4);
        const auto [l0, lp0] = burst(0, InterleaverKind::Long, 100, 5);
        std::vector<float> mixed(4000, 0.0f);
        std::vector<std::size_t> starts;
        for (const auto* a : {&l3, &a3, &l0}) {
            starts.push_back(mixed.size());
            mixed.insert(mixed.end(), a->begin(), a->end());
            mixed.insert(mixed.end(), 12000, 0.0f);
        }
        RxOptions o;
        o.max_interleaver = InterleaverKind::Short;
        Made m = make(o);
        const std::vector<Ev> ev = run(*m.rx, mixed);
        REQUIRE(count(ev, RxEventKind::BurstRefused) == 2);
        const Ev* r3 = find(ev, RxEventKind::BurstRefused, 0);
        CHECK(r3->waveform == 3);
        CHECK(r3->il == InterleaverKind::Long);
        CHECK(r3->k == 7);
        CHECK(r3->sample > static_cast<std::int64_t>(starts[0]));
        CHECK(r3->sample < static_cast<std::int64_t>(starts[1]));
        const Ev* r0 = find(ev, RxEventKind::BurstRefused, 1);
        CHECK(r0->waveform == 0);  // WN 0 is held to the same rule
        CHECK(r0->il == InterleaverKind::Long);
        CHECK(r0->sample > static_cast<std::int64_t>(starts[2]));
        CHECK(m.rx->stats().bursts_refused == 2);
        REQUIRE(count(ev, RxEventKind::BurstCompleted) == 1);
        CHECK(count(ev, RxEventKind::BurstStarted) == 1);
        CHECK(payload_of(ev, 0) == p3);
        CHECK(m.rx->state() == RxState::Searching);
        // A receiver taking the long interleaver decodes all three.
        Made all = make(RxOptions{});
        const std::vector<Ev> ev2 = run(*all.rx, mixed);
        REQUIRE(count(ev2, RxEventKind::BurstCompleted) == 3);
        CHECK(payload_of(ev2, 0) == lp3);
        CHECK(payload_of(ev2, 2) == lp0);
        CHECK(count(ev2, RxEventKind::BurstRefused) == 0);
    }
}

TEST_CASE("receiver: options, storage and misuse", "[receiver]") {
    std::size_t bytes = 0;
    RxOptions o;
    CHECK(Receiver::storage_bytes(o, bytes) == Status::Ok);
    std::size_t short_bytes = 0;
    o.max_interleaver = InterleaverKind::Short;
    REQUIRE(Receiver::storage_bytes(o, short_bytes) == Status::Ok);
    CHECK(short_bytes < bytes);
    {
        Made longest = make(RxOptions{});
        CHECK(longest.rx->max_block_bits() == rx_max_block_bits);
        CHECK(longest.rx->max_soft_values() == rx_max_soft_values);
        RxOptions s_opts;
        s_opts.max_interleaver = InterleaverKind::Short;
        Made shorter = make(s_opts);
        CHECK(shorter.rx->max_block_bits() < rx_max_block_bits);
        CHECK(shorter.rx->max_soft_values() < rx_max_soft_values);
        CHECK(shorter.rx->max_block_bits() > 0);
    }
    std::printf("receiver storage: %zu bytes for every interleaver, %zu with the short interleaver at most\n", bytes, short_bytes);
    for (const InterleaverKind il : {InterleaverKind::UltraShort, InterleaverKind::Short, InterleaverKind::Medium, InterleaverKind::Long}) {
        RxOptions x;
        x.max_interleaver = il;
        std::size_t b = 0;
        REQUIRE(Receiver::storage_bytes(x, b) == Status::Ok);
        std::printf("  max interleaver %d: %zu bytes\n", static_cast<int>(il), b);
    }

    auto bad = [](auto change) {
        RxOptions x;
        change(x);
        std::size_t b = 0;
        return Receiver::storage_bytes(x, b);
    };
    CHECK(bad([](RxOptions& x) { x.max_input_data_blocks = -1; }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.roll_off = 0.01; }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.sync_threshold = std::numeric_limits<double>::quiet_NaN(); }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.rls_forgetting_factor = std::numeric_limits<float>::infinity(); }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.track_ridge = std::numeric_limits<float>::quiet_NaN(); }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.max_interleaver = static_cast<InterleaverKind>(4); }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.turbo_noise_pricing = static_cast<TurboNoisePricing>(3); }) == Status::InvalidArgument);
    CHECK(bad([](RxOptions& x) { x.rls_forgetting_factor = 0.99f; x.track_ridge = 0.01f; }) == Status::Ok);

    // The C# defaults.
    const RxOptions d;
    CHECK(d.max_input_data_blocks == 0);
    CHECK(d.sync_threshold == 0.32);
    CHECK_FALSE(d.rls_forgetting_factor.has_value());
    CHECK_FALSE(d.disable_turbo);
    CHECK_FALSE(d.track_ridge.has_value());
    CHECK(d.turbo_noise_pricing == TurboNoisePricing::SpikeUp);
    CHECK(d.roll_off == 0.35);
    CHECK_FALSE(d.presence_release_off);
    CHECK(d.max_interleaver == InterleaverKind::Long);
    CHECK_FALSE(d.soft_decisions);

    Receiver rx;
    std::size_t used = 9;
    const std::vector<float> zeros(5000, 0.0f);
    CHECK_FALSE(rx.configured());
    CHECK(rx.process(zeros, used) == Status::State);
    CHECK(used == 0);
    CHECK(rx.next_event().kind == RxEventKind::None);
    CHECK(rx.peek_event().kind == RxEventKind::None);
    CHECK(rx.state() == RxState::Searching);
    CHECK(rx.max_block_bits() == 0);
    rx.terminate();
    rx.reset();

    std::vector<std::uint8_t> mem(short_bytes);
    CHECK(rx.configure(o, {mem.data(), mem.size() - 1}) == Status::BufferTooSmall);
    CHECK_FALSE(rx.configured());
    RxOptions wrong = o;
    wrong.roll_off = 2;
    CHECK(rx.configure(wrong, mem) == Status::InvalidArgument);
    // Any alignment.
    std::vector<std::uint8_t> odd(short_bytes + 1);
    CHECK(rx.configure(o, {odd.data() + 1, short_bytes}) == Status::Ok);
    REQUIRE(rx.configure(o, mem) == Status::Ok);
    CHECK(rx.process(zeros, used) == Status::Ok);
    CHECK(used == zeros.size());
    CHECK(rx.samples_taken() == 5000);
    CHECK(rx.stats().samples_taken == 5000);
    CHECK(rx.next_event().kind == RxEventKind::None);

    // Moving hands it over.
    Receiver moved(std::move(rx));
    CHECK(moved.configured());
    CHECK_FALSE(rx.configured());  // NOLINT(bugprone-use-after-move): moved-from is unconfigured by contract
    Receiver assigned;
    assigned = std::move(moved);
    CHECK(assigned.configured());
    CHECK(assigned.samples_taken() == 5000);
    assigned.reset();
    CHECK(assigned.samples_taken() == 0);

    // With events queued it takes no audio until they are drained.
    TxSettings s;
    s.waveform_number = 4;
    std::size_t tb = 0;
    REQUIRE(Transmitter::storage_bytes(s, 100, tb) == Status::Ok);
    std::vector<std::uint8_t> tmem(tb);
    Transmitter tx;
    REQUIRE(tx.configure(s, 100, tmem) == Status::Ok);
    std::size_t n = 0;
    REQUIRE(tx.audio_samples(100, n) == Status::Ok);
    std::vector<float> audio(n + 9600, 0.0f);
    std::size_t written = 0;
    REQUIRE(tx.modulate(std::vector<std::uint8_t>(100, 1), std::span<float>(audio.data(), n), written) == Status::Ok);
    REQUIRE(assigned.process(audio, used) == Status::Ok);
    REQUIRE(used < audio.size());  // stopped at the lock
    std::size_t again = 7;
    CHECK(assigned.process(std::span<const float>(audio.data() + used, audio.size() - used), again) == Status::State);
    CHECK(again == 0);
    CHECK(assigned.state() == RxState::Tracking);
    const RxEvent e = assigned.next_event();
    CHECK(e.kind == RxEventKind::BurstStarted);
    CHECK(e.waveform == 4);
    CHECK(e.sample == static_cast<std::int64_t>(used));
    CHECK(assigned.process(std::span<const float>(audio.data() + used, audio.size() - used), again) == Status::Ok);
}

TEST_CASE("reference receiver: each instrument option switched on, against the C#", "[receiver]") {
    const std::string stem = MS110D_RX_OPTIONS_STEM;
    const std::string corpus_stem = MS110D_RX_CORPUS_STEM;
    require_file(stem + ".txt");
    require_file(stem + ".f32le");
    require_file(corpus_stem + ".i16le");
    std::ifstream text(stem + ".txt");
    std::ifstream cut(stem + ".f32le", std::ios::binary);
    std::ifstream corpus(corpus_stem + ".i16le", std::ios::binary);
    REQUIRE((text.good() && cut.good() && corpus.good()));

    // The dump: per burst its audio, then per variant the C#'s blocks, end and counters.
    struct Variant {
        std::string name;
        std::vector<std::string> lines;  // block, end and stats lines as the C# wrote them
    };
    struct Burst {
        std::string label, source;
        long long offset = 0, samples = 0;
        std::vector<Variant> variants;
    };
    std::vector<Burst> bursts;
    std::string line;
    while (std::getline(text, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind, w;
        s >> kind;
        if (kind == "burst") {
            Burst b;
            s >> b.label >> w >> b.source >> w >> b.offset >> w >> b.samples;
            bursts.push_back(b);
        } else if (kind == "variant") {
            Variant v;
            s >> v.name;
            bursts.back().variants.push_back(v);
        } else {
            bursts.back().variants.back().lines.push_back(line);
        }
    }
    REQUIRE(bursts.size() > 40);

    auto options_for = [](const std::string& v) {
        RxOptions o;
        o.soft_decisions = true;
        o.max_interleaver = InterleaverKind::Medium;
        if (v == "rls") o.rls_forgetting_factor = 0.98f;
        else if (v == "ridge") o.track_ridge = 0.3f;
        else if (v == "nseg-off") o.turbo_noise_pricing = TurboNoisePricing::Off;
        else if (v == "nseg-2s") o.turbo_noise_pricing = TurboNoisePricing::Spike2s;
        else if (v == "presence-off") o.presence_release_off = true;
        else REQUIRE(v == "base");
        return o;
    };

    // What the C++ makes of one burst, in the dump's line format.
    auto describe = [](const std::vector<Ev>& ev, const RxStats& st) {
        std::vector<std::string> out;
        bool ended = false;
        for (const Ev& e : ev) {
            if (ended) break;
            if (e.kind == RxEventKind::BlockDecoded) {
                const Ev* f = nullptr;
                const Ev* t = nullptr;
                for (const Ev& x : ev) {
                    if (x.block == e.block && x.kind == RxEventKind::FirstPassSoft && f == nullptr) f = &x;
                    if (x.block == e.block && x.kind == RxEventKind::TurboSoft && t == nullptr) t = &x;
                }
                out.push_back("block " + std::to_string(e.block) + " " + bit_hash(e.bits) + " " +
                              (f != nullptr ? float_hash(f->soft) : "?") + " " + (t != nullptr ? float_hash(t->soft) : "-"));
            } else if (e.kind == RxEventKind::BurstCompleted) {
                out.push_back(std::string("end ") + reason_name(e.reason) + " " + std::to_string(e.blocks) + " " +
                              std::to_string(e.payload_bits));
                ended = true;
            }
        }
        if (!ended) out.push_back("end None 0 0");
        out.push_back("stats converged " + std::to_string(st.turbo_converged) + " reverted " + std::to_string(st.turbo_reverted) +
                      " salvaged " + std::to_string(st.turbo_salvaged) + " aborted " + std::to_string(st.turbo_aborted) +
                      " skipped " + std::to_string(st.turbo_skipped) + " collapse " + std::to_string(st.collapse_resolves) +
                      " absent " + std::to_string(st.signal_absent_releases) + " agc " + std::to_string(st.agc_resolves) +
                      " mfb_offered " + std::to_string(st.mfb_offered) + " mfb_selected " + std::to_string(st.mfb_selected));
        return out;
    };

    std::map<std::string, Made> receivers;
    std::map<std::string, long> runs, equal, cs_changed, agc;
    for (const Burst& b : bursts) {
        INFO("burst " << b.label);
        std::vector<float> audio(static_cast<std::size_t>(b.samples));
        if (b.source == "corpus") {
            std::vector<std::int16_t> q(audio.size());
            corpus.seekg(b.offset);
            corpus.read(reinterpret_cast<char*>(q.data()), static_cast<std::streamsize>(2 * q.size()));
            for (std::size_t i = 0; i < q.size(); ++i) audio[i] = static_cast<float>(q[i] / 8192.0);
        } else {
            cut.seekg(b.offset);
            cut.read(reinterpret_cast<char*>(audio.data()), static_cast<std::streamsize>(4 * audio.size()));
        }
        REQUIRE((corpus.good() && cut.good()));
        audio.resize(audio.size() + 9600, 0.0f);
        const Variant* base = nullptr;
        for (const Variant& v : b.variants) {
            if (v.name == "base") base = &v;
        }
        REQUIRE(base != nullptr);
        for (const Variant& v : b.variants) {
            INFO("variant " << v.name);
            auto it = receivers.find(v.name);
            if (it == receivers.end()) it = receivers.emplace(v.name, make(options_for(v.name))).first;
            Receiver& rx = *it->second.rx;
            rx.reset();
            const std::vector<Ev> ev = run(rx, audio);
            const std::vector<std::string> got = describe(ev, rx.stats());
            ++runs[v.name];
            // The default build is held to everything; the single build's soft values differ from the
            // C#'s in the last bits, so there the soft value hashes are left out (the decisions,
            // the end and the counters remain).
            const auto decisions = [](std::vector<std::string> lines) {
                if (strict) return lines;
                for (std::string& l : lines) {
                    if (l.rfind("block ", 0) != 0) continue;
                    std::istringstream in(l);
                    std::string w, index, n, hash, first, turbo;
                    in >> w >> index >> n >> hash >> first >> turbo;
                    l = "block " + index + " " + n + " " + hash + (turbo == "-" ? " -" : " turbo");
                }
                return lines;
            };
            if (decisions(got) == decisions(v.lines)) ++equal[v.name];
            if (strict) CHECK(got == v.lines);
            if (v.lines != base->lines) ++cs_changed[v.name];  // the option made a difference in the C#
            agc[v.name] += rx.stats().agc_resolves;
        }
    }
    for (const auto& [name, n] : runs) {
        std::printf("instrument option %s: %ld bursts, the C++ equal to the C# on %ld; the C# changed by the option on %ld; "
                    "level gain engaged %ld times\n",
                    name.c_str(), n, equal[name], cs_changed[name], agc[name]);
    }
    // Every option is exercised: it changes the C#'s result on some burst.
    for (const char* v : {"rls", "ridge", "nseg-off", "nseg-2s", "presence-off"}) CHECK(cs_changed[v] > 0);
    CHECK(agc["base"] >= 2);  // the weak bursts
    if (!strict) {
        // The single build's float sums may move a marginal fading burst (see dfe-receiver.md); its
        // decisions, ends and counters must still match on most.
        for (const auto& [name, n] : runs) CHECK(equal[name] * 4 >= n * 3);
    }
}
