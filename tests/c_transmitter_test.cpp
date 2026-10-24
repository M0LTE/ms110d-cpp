// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
// The transmit path through the C interface only (ms110d.h, the shared library): payload bits to
// audio against the C# reference audio (file 13) for every case of the standard set, with the
// handle made by create and, alternately, built in caller memory. Written in C++ only for the
// reference file reader; nothing but the C header is used from the library.

#include <m0lte/ms110d/ms110d.h>

#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using ms110d_test::ReferenceCase;

namespace {

int32_t interleaver_from_name(const std::string& s) {
    if (s == "ultrashort") return MS110D_INTERLEAVER_ULTRA_SHORT;
    if (s == "short") return MS110D_INTERLEAVER_SHORT;
    if (s == "medium") return MS110D_INTERLEAVER_MEDIUM;
    return MS110D_INTERLEAVER_LONG;
}

}  // namespace

TEST_CASE("reference end to end through the C interface: payload bits to audio matches all 122 cases",
          "[reference][c_transmitter]") {
    const std::filesystem::path dir = ms110d_test::reference_dir_or_skip();
    const auto index = ms110d_test::load_index(dir);
    const uint32_t chunks[] = {1, 7, 64, 1000, 4096, 100000};
    std::size_t cases = 0;
    double worst = 0;
    for (const auto& name : index.cases) {
        INFO("case " << name);
        const ReferenceCase c(dir / name, name);
        const auto& m = c.manifest();
        ms110d_tx_settings s;
        REQUIRE(ms110d_tx_settings_default(&s) == MS110D_OK);
        s.waveform_number = static_cast<int32_t>(m.integer("wn"));
        s.interleaver = interleaver_from_name(m.str("interleaver"));
        s.constraint_length = static_cast<int32_t>(m.integer("constraint_length"));
        s.tlc_blocks = static_cast<int32_t>(m.integer("tlc_blocks"));
        s.preamble_superframes = static_cast<int32_t>(m.integer("preamble_superframes"));
        s.append_eom = m.integer("append_eom") != 0 ? 1 : 0;
        s.append_eot = m.integer("append_eot") != 0 ? 1 : 0;
        s.amplitude = static_cast<float>(m.real("amplitude"));
        s.roll_off = m.real("roll_off");
        const auto payload = c.bytes("payload_bits");
        const auto count = static_cast<uint32_t>(payload.size());
        const auto ref = c.real32("audio_f32le");

        // Even cases: create/destroy. Odd cases: the caller's own memory.
        ms110d_tx* tx = nullptr;
        void* memory = nullptr;
        if (cases % 2 == 0) {
            REQUIRE(ms110d_tx_create(&s, count, &tx) == MS110D_OK);
        } else {
            uint32_t bytes = 0, alignment = 0;
            REQUIRE(ms110d_tx_memory_size(&s, count, &bytes, &alignment) == MS110D_OK);
            memory = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
            REQUIRE(memory != nullptr);
            REQUIRE(ms110d_tx_init(memory, bytes, &s, count, &tx) == MS110D_OK);
        }

        REQUIRE(ms110d_tx_push_bits(tx, payload.data(), count / 2) == MS110D_OK);
        REQUIRE(ms110d_tx_push_bits(tx, payload.data() + count / 2, count - count / 2) == MS110D_OK);
        REQUIRE(ms110d_tx_end_payload(tx) == MS110D_OK);
        uint32_t total = 0;
        REQUIRE(ms110d_tx_audio_length(tx, count, &total) == MS110D_OK);
        REQUIRE(total == ref.size());
        std::vector<float> audio(total);
        const uint32_t chunk = chunks[cases % std::size(chunks)];
        uint32_t at = 0, got = 0;
        do {
            const uint32_t room = std::min<uint32_t>(chunk, total - at);
            REQUIRE(ms110d_tx_read(tx, audio.data() + at, room, &got) == MS110D_OK);
            at += got;
        } while (got != 0);
        REQUIRE(at == total);

        double err = 0;
        for (std::size_t i = 0; i < ref.size(); ++i) {
            err = std::max(err, std::fabs(static_cast<double>(audio[i]) - static_cast<double>(ref[i])));
        }
        worst = std::max(worst, err);
        CHECK(err <= 1e-6);

        if (memory != nullptr) {
            std::free(memory);
        } else {
            ms110d_tx_destroy(tx);
        }
        ++cases;
    }
    CHECK(cases == 122);
    INFO("worst error " << worst);
}
