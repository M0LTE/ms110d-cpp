// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The receive path through the C interface only (ms110d.h, the shared library): every reference
// case decoded and held to the C# receiver's blocks and end (the rx-turbo dump, and the WN 0 dump),
// with the handle made by create and, alternately, built in caller memory; then the event buffer and
// struct rules. Written in C++ only for the reference file reader; nothing but the C header is used
// from the library.

#include <m0lte/ms110d/ms110d.h>

#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string bit_hash(const std::vector<std::uint8_t>& bits) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const std::uint8_t b : bits) {
        const std::uint32_t v = b;
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ULL;
        }
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return std::to_string(bits.size()) + " " + buf;
}

struct Expected {
    int wn = -1;
    std::vector<std::string> info;     // per block "<n> <hash>"
    std::vector<long long> at;         // per block T/2 sample
    std::string reason, payload_hash;  // reason "None" when the burst did not end
    int blocks = 0;
    long long payload_bits = 0, end_at = 0;
};

std::map<std::string, Expected> load_turbo(const char* path) {
    std::map<std::string, Expected> out;
    std::ifstream in(path);
    std::string line, label;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string kind, w;
        s >> kind;
        if (kind == "case") {
            s >> label >> w >> out[label].wn;
        } else if (kind == "block") {
            int index = 0;
            long long at = 0;
            s >> index >> at;
            out[label].at.push_back(at);
        } else if (kind == "info") {
            std::string n, h;
            s >> n >> h;
            out[label].info.push_back(n + " " + h);
        } else if (kind == "end") {
            std::string n, h;
            Expected& e = out[label];
            s >> e.reason >> e.blocks >> e.payload_bits >> e.end_at >> n >> h;
            e.payload_hash = n + " " + h;
        }
    }
    return out;
}

const char* reason_name(int32_t r) {
    switch (r) {
        case MS110D_END_EOM: return "Eom";
        case MS110D_END_MAX_INPUT_DATA_BLOCKS: return "MaxInputDataBlocks";
        case MS110D_END_TERMINATED: return "Terminated";
        case MS110D_END_SIGNAL_LOST: return "SignalLost";
        case MS110D_END_SIGNAL_ABSENT: return "SignalAbsent";
        default: return "?";
    }
}

struct Got {
    int started_wn = -1;
    std::vector<std::vector<std::uint8_t>> blocks;
    std::vector<long long> block_samples;
    bool ended = false;
    int32_t reason = -1, blocks_count = 0;
    long long payload_bits = 0, end_sample = 0;
    std::vector<std::uint8_t> payload;
};

Got run(ms110d_rx* rx, const std::vector<float>& audio, uint32_t chunk) {
    Got g;
    std::vector<std::uint8_t> bits(49152);
    uint32_t at = 0;
    while (at < audio.size()) {
        const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(audio.size()) - at);
        uint32_t used = 0;
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, n, &used) == MS110D_OK);
        at += used;
        ms110d_rx_event e;
        e.struct_size = sizeof e;
        while (ms110d_rx_next_event(rx, &e, bits.data(), static_cast<uint32_t>(bits.size()), nullptr, 0) == MS110D_OK &&
               e.kind != MS110D_RX_EVENT_NONE) {
            if (e.kind == MS110D_RX_EVENT_BURST_STARTED && g.started_wn < 0) g.started_wn = e.waveform_number;
            if (e.kind == MS110D_RX_EVENT_BLOCK_DECODED && !g.ended) {
                g.blocks.emplace_back(bits.begin(), bits.begin() + e.length);
                g.block_samples.push_back(e.sample);
            }
            if (e.kind == MS110D_RX_EVENT_BURST_COMPLETED && !g.ended) {
                g.ended = true;
                g.reason = e.end_reason;
                g.blocks_count = e.blocks;
                g.payload_bits = e.payload_bits;
                g.end_sample = e.sample;
                for (const auto& b : g.blocks) g.payload.insert(g.payload.end(), b.begin(), b.end());
                g.payload.resize(static_cast<std::size_t>(e.payload_bits));
            }
        }
    }
    return g;
}

struct Handle {
    ms110d_rx* rx = nullptr;
    void* raw = nullptr;
    bool created = false;
    ~Handle() {
        if (created) ms110d_rx_destroy(rx);
        std::free(raw);
    }
};

void make(Handle& h, const ms110d_rx_options& o, bool create) {
    if (create) {
        REQUIRE(ms110d_rx_create(&o, &h.rx) == MS110D_OK);
        h.created = true;
        return;
    }
    uint32_t bytes = 0, alignment = 0;
    REQUIRE(ms110d_rx_memory_size(&o, &bytes, &alignment) == MS110D_OK);
    h.raw = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
    REQUIRE(h.raw != nullptr);
    REQUIRE(ms110d_rx_init(h.raw, bytes, &o, &h.rx) == MS110D_OK);
}

void require_file(const std::string& path) {
    if (std::ifstream(path).good()) return;
    if (std::getenv("MS110D_REQUIRE_REFERENCE") != nullptr) FAIL("missing: " << path);
    SKIP("no file at " << path << " (run scripts/generate-rx-dumps.sh)");
}

}  // namespace

TEST_CASE("reference receive through the C interface: every case decodes as the C# does", "[reference][c_receiver]") {
    const std::filesystem::path dir = ms110d_test::standard_reference_dir_or_skip();
    require_file(MS110D_CSHARP_RX_TURBO);
    const auto expected = load_turbo(MS110D_CSHARP_RX_TURBO);
    const auto index = ms110d_test::load_index(dir);
    ms110d_rx_options o;
    REQUIRE(ms110d_rx_options_default(&o) == MS110D_OK);
    Handle created, built;
    make(created, o, true);
    make(built, o, false);
    const uint32_t chunks[] = {2400, 1, 333, 9600, 100000};
    std::size_t cases = 0, eom = 0;
    for (const auto& name : index.cases) {
        INFO("case " << name);
        const ms110d_test::ReferenceCase c(dir / name, name);
        std::vector<float> audio = c.standard_real32("audio_f32le");
        audio.resize(audio.size() + 9600, 0.0f);
        const auto sent = c.bytes("payload_bits");
        ms110d_rx* rx = (cases % 2 == 0) ? created.rx : built.rx;
        REQUIRE(ms110d_rx_reset(rx) == MS110D_OK);
        const Got g = run(rx, audio, chunks[cases % 5]);
        ++cases;
        const int wn = static_cast<int>(c.manifest().integer("wn"));
        if (wn == 0) {
            // The WN 0 dump has the decoded bits; the transmitted payload is what they come to.
            REQUIRE(g.ended);
            CHECK(g.started_wn == 0);
            CHECK(g.reason == MS110D_END_EOM);
            CHECK(g.payload == sent);
            ++eom;
            continue;
        }
        const auto it = expected.find(name);
        REQUIRE(it != expected.end());
        const Expected& e = it->second;
        if (e.wn < 0) {
            CHECK(g.started_wn < 0);
            continue;
        }
        CHECK(g.started_wn == e.wn);
        REQUIRE(g.blocks.size() >= e.info.size());
        for (std::size_t i = 0; i < e.info.size(); ++i) {
            CHECK(bit_hash(g.blocks[i]) == e.info[i]);
            CHECK(g.block_samples[i] == 2 * e.at[i]);
        }
        if (e.reason == "None") {
            CHECK_FALSE(g.ended);
            continue;
        }
        REQUIRE(g.ended);
        CHECK(std::string(reason_name(g.reason)) == e.reason);
        CHECK(g.blocks_count == e.blocks);
        CHECK(g.payload_bits == e.payload_bits);
        CHECK(g.end_sample == 2 * e.end_at);
        CHECK(bit_hash(g.payload) == e.payload_hash);
        if (e.reason == "Eom") {
            CHECK(g.payload == sent);
            ++eom;
        }
    }
    CHECK(cases == 122);
    CHECK(eom == 119);
}

TEST_CASE("C receive: buffers, struct sizes and options", "[c_receiver]") {
    ms110d_rx_options o;
    REQUIRE(ms110d_rx_options_default(&o) == MS110D_OK);
    CHECK(o.struct_size == sizeof(ms110d_rx_options));
    CHECK(o.max_input_data_blocks == 0);
    CHECK(o.sync_threshold == 0.32);
    CHECK(o.roll_off == 0.35);
    CHECK(o.max_interleaver == MS110D_INTERLEAVER_LONG);
    CHECK(o.turbo_noise_pricing == MS110D_NOISE_PRICING_SPIKE_UP);
    CHECK(o.soft_decisions == 0);
    CHECK(o.has_rls_forgetting_factor == 0);
    CHECK(o.has_track_ridge == 0);

    ms110d_rx* rx = nullptr;
    uint32_t bytes = 0, alignment = 0;
    // Options the library must refuse.
    auto refused = [&](auto change) {
        ms110d_rx_options x = o;
        change(x);
        return ms110d_rx_memory_size(&x, &bytes, &alignment);
    };
    CHECK(refused([](ms110d_rx_options& x) { x.reserved = 1; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.struct_size = 8; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.soft_decisions = 2; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.max_interleaver = 4; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.turbo_noise_pricing = -1; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.max_input_data_blocks = -1; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(refused([](ms110d_rx_options& x) { x.roll_off = 0; }) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_memory_size(nullptr, &bytes, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_memory_size(&o, nullptr, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_create(&o, nullptr) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_options_default(nullptr) == MS110D_ERR_INVALID_ARGUMENT);

    // A larger struct from a newer caller: accepted only with zeros past the known fields.
    struct Bigger {
        ms110d_rx_options base;
        uint32_t extra;
        uint32_t pad;
    } big{};
    big.base = o;
    big.base.struct_size = sizeof big;
    CHECK(ms110d_rx_memory_size(&big.base, &bytes, &alignment) == MS110D_OK);
    big.extra = 1;
    CHECK(ms110d_rx_memory_size(&big.base, &bytes, &alignment) == MS110D_ERR_INVALID_ARGUMENT);

    // Caller memory: too small or misaligned.
    o.max_interleaver = MS110D_INTERLEAVER_SHORT;
    o.soft_decisions = 1;
    REQUIRE(ms110d_rx_memory_size(&o, &bytes, &alignment) == MS110D_OK);
    std::unique_ptr<std::uint8_t, decltype(&std::free)> mem(static_cast<std::uint8_t*>(std::aligned_alloc(alignment, bytes + alignment)),
                                                           &std::free);
    CHECK(ms110d_rx_init(mem.get(), bytes - 1, &o, &rx) == MS110D_ERR_BAD_MEMORY);
    CHECK(ms110d_rx_init(mem.get() + 1, bytes, &o, &rx) == MS110D_ERR_BAD_MEMORY);
    CHECK(ms110d_rx_init(nullptr, bytes, &o, &rx) == MS110D_ERR_BAD_MEMORY);
    REQUIRE(ms110d_rx_init(mem.get(), bytes, &o, &rx) == MS110D_OK);

    // A burst made by the transmitter.
    ms110d_tx_settings s;
    REQUIRE(ms110d_tx_settings_default(&s) == MS110D_OK);
    s.waveform_number = 3;
    ms110d_tx* tx = nullptr;
    REQUIRE(ms110d_tx_create(&s, 2000, &tx) == MS110D_OK);
    std::vector<std::uint8_t> payload(2000);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>((i * 7 / 3) & 1);
    uint32_t n = 0;
    REQUIRE(ms110d_tx_audio_length(tx, 2000, &n) == MS110D_OK);
    std::vector<float> audio(n + 9600, 0.0f);
    uint32_t written = 0;
    REQUIRE(ms110d_tx_modulate(tx, payload.data(), 2000, audio.data(), n, &written) == MS110D_OK);
    ms110d_tx_destroy(tx);

    int32_t state = -1;
    CHECK(ms110d_rx_state(rx, &state) == MS110D_OK);
    CHECK(state == MS110D_RX_STATE_SEARCHING);
    CHECK(ms110d_rx_state(rx, nullptr) == MS110D_ERR_INVALID_ARGUMENT);
    uint32_t used = 99;
    CHECK(ms110d_rx_process(rx, nullptr, 5, &used) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(used == 0);
    CHECK(ms110d_rx_process(rx, audio.data(), 5, nullptr) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_process(nullptr, audio.data(), 5, &used) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_process(rx, nullptr, 0, &used) == MS110D_OK);

    ms110d_rx_event ev;
    ev.struct_size = sizeof ev;
    CHECK(ms110d_rx_next_event(rx, &ev, nullptr, 0, nullptr, 0) == MS110D_OK);
    CHECK(ev.kind == MS110D_RX_EVENT_NONE);
    ev.struct_size = 16;
    CHECK(ms110d_rx_next_event(rx, &ev, nullptr, 0, nullptr, 0) == MS110D_ERR_INVALID_ARGUMENT);
    ev.struct_size = sizeof ev;
    CHECK(ms110d_rx_next_event(rx, nullptr, nullptr, 0, nullptr, 0) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_next_event(rx, &ev, nullptr, 4, nullptr, 0) == MS110D_ERR_INVALID_ARGUMENT);

    std::vector<std::uint8_t> bits(4096);
    std::vector<float> soft(8192);
    std::vector<std::uint8_t> got;
    bool soft_tried = false, pending_tried = false, ended = false;
    int64_t payload_bits = -1;
    uint32_t at = 0;
    while (at < audio.size() && !ended) {
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, static_cast<uint32_t>(audio.size()) - at, &used) == MS110D_OK);
        at += used;
        if (!pending_tried && at < audio.size()) {
            // Events queued: no more audio until they are taken.
            uint32_t none = 5;
            CHECK(ms110d_rx_process(rx, audio.data() + at, 10, &none) == MS110D_ERR_STATE);
            CHECK(none == 0);
            pending_tried = true;
        }
        for (;;) {
            ev.struct_size = sizeof ev;
            const ms110d_status st = ms110d_rx_next_event(rx, &ev, bits.data(), static_cast<uint32_t>(bits.size()), soft.data(),
                                                    static_cast<uint32_t>(soft.size()));
            REQUIRE(st == MS110D_OK);
            if (ev.kind == MS110D_RX_EVENT_NONE) break;
            if (ev.kind == MS110D_RX_EVENT_BLOCK_DECODED) got.insert(got.end(), bits.begin(), bits.begin() + ev.length);
            if (ev.kind == MS110D_RX_EVENT_FIRST_PASS_SOFT) soft_tried = true;
            if (ev.kind == MS110D_RX_EVENT_BURST_COMPLETED) {
                CHECK(ev.end_reason == MS110D_END_EOM);
                CHECK(ev.waveform_number == 3);
                payload_bits = ev.payload_bits;
                ended = true;
            }
        }
    }
    REQUIRE(ended);
    got.resize(static_cast<std::size_t>(payload_bits));
    CHECK(got == payload);
    CHECK(soft_tried);
    CHECK(pending_tried);

    // A too-small buffer leaves the event queued; a larger struct is zeroed past the known fields.
    REQUIRE(ms110d_rx_reset(rx) == MS110D_OK);
    struct BigEvent {
        ms110d_rx_event base;
        uint32_t extra;
        uint32_t pad;
    } bev;
    std::memset(&bev, 0xAB, sizeof bev);
    at = 0;
    bool checked = false;
    while (at < audio.size() && !checked) {
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, static_cast<uint32_t>(audio.size()) - at, &used) == MS110D_OK);
        at += used;
        for (;;) {
            bev.base.struct_size = sizeof bev;
            const ms110d_status st = ms110d_rx_next_event(rx, &bev.base, bits.data(), 3, soft.data(), 2);
            if (st == MS110D_ERR_BUFFER_TOO_SMALL) {
                CHECK(bev.extra == 0);
                const uint32_t need = bev.base.length;
                const uint32_t kind = bev.base.kind;
                CHECK(need > 3);
                // Still queued: the same event comes back, now with room.
                ev.struct_size = sizeof ev;
                REQUIRE(ms110d_rx_next_event(rx, &ev, bits.data(), static_cast<uint32_t>(bits.size()), soft.data(),
                                             static_cast<uint32_t>(soft.size())) == MS110D_OK);
                CHECK(ev.kind == kind);
                CHECK(ev.length == need);
                checked = true;
                break;
            }
            REQUIRE(st == MS110D_OK);
            if (bev.base.kind == MS110D_RX_EVENT_NONE) break;
        }
    }
    CHECK(checked);

    // Terminate mid-burst.
    REQUIRE(ms110d_rx_reset(rx) == MS110D_OK);
    at = 0;
    bool terminated = false, started = false;
    while (at < audio.size() && !terminated) {
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, std::min<uint32_t>(960, static_cast<uint32_t>(audio.size()) - at), &used) ==
                MS110D_OK);
        at += used;
        ev.struct_size = sizeof ev;
        while (ms110d_rx_next_event(rx, &ev, bits.data(), static_cast<uint32_t>(bits.size()), soft.data(),
                                    static_cast<uint32_t>(soft.size())) == MS110D_OK &&
               ev.kind != MS110D_RX_EVENT_NONE) {
            if (ev.kind == MS110D_RX_EVENT_BURST_STARTED) {
                started = true;
                CHECK(ms110d_rx_terminate(rx) == MS110D_OK);
            }
            if (ev.kind == MS110D_RX_EVENT_BURST_COMPLETED) {
                CHECK(ev.end_reason == MS110D_END_TERMINATED);
                CHECK(ev.blocks == 0);
                terminated = true;
            }
        }
    }
    CHECK(started);
    CHECK(terminated);
    CHECK(ms110d_rx_state(rx, &state) == MS110D_OK);
    CHECK(state == MS110D_RX_STATE_SEARCHING);
    // Counters: the C++ ones, through a struct with struct_size.
    ms110d_rx_stats st;
    st.struct_size = sizeof st;
    REQUIRE(ms110d_rx_get_stats(rx, &st) == MS110D_OK);
    CHECK(st.samples_taken == static_cast<int64_t>(at));
    CHECK(st.turbo_converged == 0);  // counts restart at reset; the terminated burst decoded no block
    CHECK(st.peak_search_metric > 0.3);
    CHECK(st.bursts_refused == 0);
    CHECK(st.reserved == 0);
    st.struct_size = 8;
    CHECK(ms110d_rx_get_stats(rx, &st) == MS110D_ERR_INVALID_ARGUMENT);
    struct BigStats {
        ms110d_rx_stats base;
        int64_t extra;
    } bst;
    std::memset(&bst, 0xCD, sizeof bst);
    bst.base.struct_size = sizeof bst;
    REQUIRE(ms110d_rx_get_stats(rx, &bst.base) == MS110D_OK);
    CHECK(bst.extra == 0);
    CHECK(ms110d_rx_get_stats(nullptr, &st) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_get_stats(rx, nullptr) == MS110D_ERR_INVALID_ARGUMENT);

    // Buffer sizes for this handle (short interleaver at most) and the global maxima.
    uint32_t need_bits = 0, need_soft = 0;
    REQUIRE(ms110d_rx_buffer_sizes(rx, &need_bits, &need_soft) == MS110D_OK);
    CHECK(need_bits > 0);
    CHECK(need_bits < MS110D_RX_MAX_BLOCK_BITS);
    CHECK(need_soft < MS110D_RX_MAX_SOFT_VALUES);
    CHECK(need_soft >= need_bits);
    CHECK(ms110d_rx_buffer_sizes(nullptr, &need_bits, &need_soft) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_buffer_sizes(rx, nullptr, &need_soft) == MS110D_ERR_INVALID_ARGUMENT);

    // Dropping events without a buffer: the burst goes by with every event dropped.
    REQUIRE(ms110d_rx_reset(rx) == MS110D_OK);
    at = 0;
    while (at < audio.size()) {
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, static_cast<uint32_t>(audio.size()) - at, &used) == MS110D_OK);
        at += used;
        for (int guard = 0; guard < 8; ++guard) REQUIRE(ms110d_rx_drop_event(rx) == MS110D_OK);
    }
    REQUIRE(ms110d_rx_get_stats(rx, &st) == MS110D_ERR_INVALID_ARGUMENT);  // struct_size is still 8
    st.struct_size = sizeof st;
    REQUIRE(ms110d_rx_get_stats(rx, &st) == MS110D_OK);
    CHECK(st.turbo_converged >= 1);
    CHECK(ms110d_rx_drop_event(nullptr) == MS110D_ERR_INVALID_ARGUMENT);

    CHECK(ms110d_rx_terminate(nullptr) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_reset(nullptr) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_rx_destroy(nullptr);
}

TEST_CASE("C receive: a burst with a longer interleaver than accepted raises BURST_REFUSED", "[c_receiver]") {
    ms110d_tx_settings s;
    REQUIRE(ms110d_tx_settings_default(&s) == MS110D_OK);
    s.waveform_number = 3;
    s.interleaver = MS110D_INTERLEAVER_LONG;
    ms110d_tx* tx = nullptr;
    REQUIRE(ms110d_tx_create(&s, 200, &tx) == MS110D_OK);
    std::vector<std::uint8_t> payload(200, 1);
    uint32_t n = 0, written = 0;
    REQUIRE(ms110d_tx_audio_length(tx, 200, &n) == MS110D_OK);
    std::vector<float> audio(3000 + n + 9600, 0.0f);
    REQUIRE(ms110d_tx_modulate(tx, payload.data(), 200, audio.data() + 3000, n, &written) == MS110D_OK);
    ms110d_tx_destroy(tx);

    ms110d_rx_options o;
    REQUIRE(ms110d_rx_options_default(&o) == MS110D_OK);
    o.max_interleaver = MS110D_INTERLEAVER_SHORT;
    ms110d_rx* rx = nullptr;
    REQUIRE(ms110d_rx_create(&o, &rx) == MS110D_OK);
    uint32_t at = 0, used = 0;
    int refused = 0, started = 0;
    ms110d_rx_event ev;
    while (at < audio.size()) {
        REQUIRE(ms110d_rx_process(rx, audio.data() + at, static_cast<uint32_t>(audio.size()) - at, &used) == MS110D_OK);
        at += used;
        ev.struct_size = sizeof ev;
        while (ms110d_rx_next_event(rx, &ev, nullptr, 0, nullptr, 0) == MS110D_OK && ev.kind != MS110D_RX_EVENT_NONE) {
            if (ev.kind == MS110D_RX_EVENT_BURST_STARTED) ++started;
            if (ev.kind == MS110D_RX_EVENT_BURST_REFUSED) {
                ++refused;
                CHECK(ev.waveform_number == 3);
                CHECK(ev.interleaver == MS110D_INTERLEAVER_LONG);
                CHECK(ev.constraint_length == 7);
                CHECK(ev.sample > 3000);
            }
            ev.struct_size = sizeof ev;
        }
    }
    CHECK(refused == 1);
    CHECK(started == 0);
    ms110d_rx_stats st;
    st.struct_size = sizeof st;
    REQUIRE(ms110d_rx_get_stats(rx, &st) == MS110D_OK);
    CHECK(st.bursts_refused == 1);
    ev.struct_size = 0;
    CHECK(ms110d_rx_next_event(rx, &ev, nullptr, 0, nullptr, 0) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_rx_destroy(rx);
}
