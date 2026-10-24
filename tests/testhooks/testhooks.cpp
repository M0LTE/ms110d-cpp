// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// TEST ONLY: see ms110d_testhooks.h. Thin wrappers over the internal stages; each checks its
// arguments, never allocates and never throws.

#include "ms110d_testhooks.h"

#include <m0lte/ms110d/mode.hpp>

#include "fec/convolutional.hpp"
#include "interleaver.hpp"
#include "puncture.hpp"
#include "scrambler.hpp"
#include "tables.hpp"
#include "trinomial_scrambler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

using namespace m0lte;
using namespace m0lte::ms110d;

const fec::ConvolutionalCode* code_for(int32_t constraint_length) {
    if (constraint_length == 7) return &fec::k7_133_171;
    if (constraint_length == 9) return &fec::k9_561_753;
    return nullptr;
}

}  // namespace

extern "C" {

int32_t ms110d_th_encode(int32_t constraint_length, const uint8_t* info, uint32_t info_len, uint8_t* coded,
                         uint32_t coded_capacity, uint32_t* coded_len) {
    const fec::ConvolutionalCode* code = code_for(constraint_length);
    if (code == nullptr || info == nullptr || coded == nullptr || coded_len == nullptr) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    const std::size_t need = 2u * static_cast<std::size_t>(info_len);
    *coded_len = static_cast<uint32_t>(need);
    if (coded_capacity < need) return MS110D_ERR_BUFFER_TOO_SMALL;
    if (!fec::encode_tail_biting(*code, std::span<const uint8_t>(info, info_len), std::span<uint8_t>(coded, need))) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    return MS110D_OK;
}

int32_t ms110d_th_puncture(int32_t constraint_length, int32_t rate_num, int32_t rate_den, const uint8_t* coded,
                           uint32_t coded_len, uint8_t* out, uint32_t out_capacity, uint32_t* out_len) {
    const fec::ConvolutionalCode* code = code_for(constraint_length);
    if (code == nullptr || coded == nullptr || out == nullptr || out_len == nullptr || rate_num <= 0 || rate_den <= 0) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    const PunctureSpec* spec = find_puncture(*code, static_cast<unsigned>(rate_num), static_cast<unsigned>(rate_den));
    if (spec == nullptr) return MS110D_ERR_UNSUPPORTED;
    const auto len = punctured_length(*spec, coded_len / 2u);
    if (!len) return MS110D_ERR_INVALID_ARGUMENT;
    *out_len = static_cast<uint32_t>(*len);
    if (out_capacity < *len) return MS110D_ERR_BUFFER_TOO_SMALL;
    const auto written = apply_puncture(*spec, std::span<const uint8_t>(coded, coded_len), std::span<uint8_t>(out, out_capacity));
    if (!written || *written != *len) return MS110D_ERR_INTERNAL;
    return MS110D_OK;
}

int32_t ms110d_th_interleave(uint32_t size_bits, uint32_t increment, const uint8_t* in, uint32_t in_len,
                             uint8_t* out, uint32_t out_capacity) {
    if (in == nullptr || out == nullptr || size_bits == 0 || in_len != size_bits) return MS110D_ERR_INVALID_ARGUMENT;
    if (out_capacity < size_bits) return MS110D_ERR_BUFFER_TOO_SMALL;
    const Interleaver il(size_bits, increment);
    if (!il.valid()) return MS110D_ERR_INVALID_ARGUMENT;
    il.interleave(in, out);
    return MS110D_OK;
}

int32_t ms110d_th_transcode(int32_t wn, const uint8_t* bits, uint32_t in_len, uint8_t* symbols,
                            uint32_t symbols_capacity, uint32_t* symbols_len) {
    if (bits == nullptr || symbols == nullptr || symbols_len == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    const auto mode = mode_3k(wn);
    if (!mode || mode->modulation == Modulation::Walsh) return MS110D_ERR_UNSUPPORTED;
    const uint32_t bps = static_cast<uint32_t>(mode->bits_per_symbol);
    if (in_len % bps != 0) return MS110D_ERR_INVALID_ARGUMENT;
    const uint32_t n = in_len / bps;
    *symbols_len = n;
    if (symbols_capacity < n) return MS110D_ERR_BUFFER_TOO_SMALL;
    for (uint32_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (uint32_t b = 0; b < bps; ++b) v = (v << 1) | (bits[i * bps + b] & 1u);
        uint8_t s = 0;
        switch (mode->modulation) {
            case Modulation::Bpsk: s = tables::transcode_bpsk[v]; break;
            case Modulation::Qpsk: s = tables::transcode_qpsk[v]; break;
            case Modulation::Psk8: s = tables::transcode_8psk[v]; break;
            case Modulation::Qam16: s = static_cast<uint8_t>(v); break;
            case Modulation::Walsh: break;
        }
        symbols[i] = s;
    }
    return MS110D_OK;
}

int32_t ms110d_th_scramble(int32_t wn, uint32_t frame_symbols, const uint8_t* symbols, uint32_t symbols_len,
                           uint8_t* out, uint32_t out_capacity) {
    if (symbols == nullptr || out == nullptr || frame_symbols == 0) return MS110D_ERR_INVALID_ARGUMENT;
    const auto mode = mode_3k(wn);
    if (!mode || mode->modulation == Modulation::Walsh) return MS110D_ERR_UNSUPPORTED;
    if (out_capacity < symbols_len) return MS110D_ERR_BUFFER_TOO_SMALL;
    const bool qam = mode->modulation == Modulation::Qam16;
    Scrambler s;
    uint32_t in_frame = 0;
    for (uint32_t i = 0; i < symbols_len; ++i) {
        if (in_frame == 0) s.reset();
        out[i] = static_cast<uint8_t>(qam ? s.next_qam(symbols[i], mode->bits_per_symbol) : s.next_psk(symbols[i]));
        if (++in_frame == frame_symbols) in_frame = 0;
    }
    return MS110D_OK;
}

int32_t ms110d_th_trinomial(uint32_t block_symbols, uint32_t count, uint8_t* out, uint32_t out_capacity) {
    if (out == nullptr || block_symbols == 0) return MS110D_ERR_INVALID_ARGUMENT;
    if (out_capacity < count) return MS110D_ERR_BUFFER_TOO_SMALL;
    Trinomial159Scrambler s;
    uint32_t in_block = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (in_block == 0) s.reset();
        out[i] = static_cast<uint8_t>(s.next());
        if (++in_block == block_symbols) in_block = 0;
    }
    return MS110D_OK;
}

}  // extern "C"
