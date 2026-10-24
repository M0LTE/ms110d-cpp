// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/mode.hpp>
#include <m0lte/ms110d/transmitter.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const auto v = m0lte::ms110d::version();
    const m0lte::ms110d::real one = 1;
    char text[64];
    std::snprintf(text, sizeof text, "%d.%d.%d", v.major, v.minor, v.patch);
    std::printf("ms110d %s (sizeof real %zu)\n", text, sizeof(one));
    // The mode tables must be reachable through the installed library.
    const auto mode = m0lte::ms110d::mode_3k(6);
    const auto il = m0lte::ms110d::interleaver_3k(6, m0lte::ms110d::InterleaverKind::Short);
    if (!mode || !il || mode->bps != 3200 || il->size_bits != 2048) return 2;
    // The transmitter must be usable through the installed headers and library.
    m0lte::ms110d::TxSettings settings;
    std::size_t bytes = 0;
    if (m0lte::ms110d::Transmitter::storage_bytes(settings, 64, bytes) != m0lte::ms110d::Status::Ok) return 3;
    std::vector<std::uint8_t> memory(bytes);
    m0lte::ms110d::Transmitter tx;
    const std::vector<std::uint8_t> bits(64, 1);
    std::size_t expected = 0, got = 0, total = 0;
    float audio[256];
    if (tx.configure(settings, 64, memory) != m0lte::ms110d::Status::Ok || tx.push_bits(bits) != m0lte::ms110d::Status::Ok ||
        tx.end_payload() != m0lte::ms110d::Status::Ok || tx.audio_samples(64, expected) != m0lte::ms110d::Status::Ok) {
        return 4;
    }
    while (!tx.done()) {
        if (tx.read(audio, got) != m0lte::ms110d::Status::Ok || got == 0) return 5;
        total += got;
    }
    if (total != expected) return 6;
    return std::strcmp(text, EXPECTED_VERSION) == 0 ? 0 : 1;
}
