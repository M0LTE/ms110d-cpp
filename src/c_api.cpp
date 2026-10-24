// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <m0lte/ms110d/ms110d.h>

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/transmitter.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>

// The only translation unit that defines the C interface. Nothing here may
// throw or allocate; the core has no exceptions anyway.

namespace {

using m0lte::ms110d::Status;
using m0lte::ms110d::Transmitter;
using m0lte::ms110d::TxSettings;

// A transmitter handle is a Transmitter at the start of its memory block, aligned to 16 bytes,
// with the storage it needs after it.
constexpr std::size_t block_alignment = 16;
constexpr std::size_t header_bytes = (sizeof(Transmitter) + block_alignment - 1) / block_alignment * block_alignment;

ms110d_status to_c(Status s) noexcept {
    return static_cast<ms110d_status>(s);
}

// Reads and checks the caller's settings. struct_size policy (docs/dev/c-interface.md): smaller
// than the first published size is an error; larger than this library knows is accepted only if
// every byte past the known size is zero, so a field this library does not know cannot be
// silently ignored. The reserved word must be zero too.
ms110d_status settings_from_c(const ms110d_tx_settings* in, TxSettings& out) noexcept {
    if (in == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    if (in->struct_size < sizeof(ms110d_tx_settings)) return MS110D_ERR_INVALID_ARGUMENT;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(in);
    for (std::size_t i = sizeof(ms110d_tx_settings); i < in->struct_size; ++i) {
        if (raw[i] != 0) return MS110D_ERR_INVALID_ARGUMENT;
    }
    if (in->reserved != 0) return MS110D_ERR_INVALID_ARGUMENT;
    if (in->interleaver < MS110D_INTERLEAVER_ULTRA_SHORT || in->interleaver > MS110D_INTERLEAVER_LONG) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    if ((in->append_eom != 0 && in->append_eom != 1) || (in->append_eot != 0 && in->append_eot != 1)) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    out.waveform_number = in->waveform_number;
    out.interleaver = static_cast<m0lte::ms110d::InterleaverKind>(in->interleaver);
    out.constraint_length = in->constraint_length;
    out.preamble_superframes = in->preamble_superframes;
    out.tlc_blocks = in->tlc_blocks;
    out.append_eom = in->append_eom != 0;
    out.append_eot = in->append_eot != 0;
    out.amplitude = in->amplitude;
    out.roll_off = in->roll_off;
    return MS110D_OK;
}

// Bytes of one block, header included, for settings already converted.
ms110d_status block_bytes(const TxSettings& s, uint32_t max_payload_bits, std::size_t& total) noexcept {
    std::size_t storage = 0;
    const Status st = Transmitter::storage_bytes(s, max_payload_bits, storage);
    if (st != Status::Ok) return to_c(st);
    total = header_bytes + storage;
    if (total > std::numeric_limits<uint32_t>::max()) return MS110D_ERR_UNSUPPORTED;
    return MS110D_OK;
}

ms110d_status build_in(void* memory, std::size_t bytes, const TxSettings& s, uint32_t max_payload_bits,
                       ms110d_tx** out) noexcept {
    auto* tx = ::new (memory) Transmitter();
    const Status st = tx->configure(s, max_payload_bits,
                                    std::span<std::uint8_t>(static_cast<std::uint8_t*>(memory) + header_bytes, bytes - header_bytes));
    if (st != Status::Ok) return to_c(st);
    *out = reinterpret_cast<ms110d_tx*>(tx);
    return MS110D_OK;
}

Transmitter* tx_of(ms110d_tx* h) noexcept {
    return reinterpret_cast<Transmitter*>(h);
}
const Transmitter* tx_of(const ms110d_tx* h) noexcept {
    return reinterpret_cast<const Transmitter*>(h);
}

}  // namespace

extern "C" {

uint32_t ms110d_abi_version(void) {
    return MS110D_ABI_VERSION;
}

ms110d_status ms110d_version(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major == nullptr || minor == nullptr || patch == nullptr) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    const auto v = m0lte::ms110d::version();
    *major = v.major;
    *minor = v.minor;
    *patch = v.patch;
    return MS110D_OK;
}

ms110d_status ms110d_precision_bits(int32_t* precision) {
    if (precision == nullptr) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    static_assert(sizeof(m0lte::ms110d::real) == 4 || sizeof(m0lte::ms110d::real) == 8, "real must be float or double");
    *precision = sizeof(m0lte::ms110d::real) == 4 ? MS110D_PRECISION_SINGLE : MS110D_PRECISION_DOUBLE;
    return MS110D_OK;
}

const char* ms110d_status_string(ms110d_status status) {
    switch (status) {
    case MS110D_OK: return "ok";
    case MS110D_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MS110D_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case MS110D_ERR_UNSUPPORTED: return "unsupported";
    case MS110D_ERR_STATE: return "invalid state";
    case MS110D_ERR_INTERNAL: return "internal error";
    case MS110D_ERR_OUT_OF_MEMORY: return "out of memory";
    case MS110D_ERR_BAD_MEMORY: return "supplied memory too small or misaligned";
    }
    return "unknown status";
}

ms110d_status ms110d_tx_settings_default(ms110d_tx_settings* settings) {
    if (settings == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    const TxSettings d;
    settings->struct_size = sizeof(ms110d_tx_settings);
    settings->waveform_number = d.waveform_number;
    settings->interleaver = static_cast<int32_t>(d.interleaver);
    settings->constraint_length = d.constraint_length;
    settings->preamble_superframes = d.preamble_superframes;
    settings->tlc_blocks = d.tlc_blocks;
    settings->append_eom = d.append_eom ? 1 : 0;
    settings->append_eot = d.append_eot ? 1 : 0;
    settings->amplitude = d.amplitude;
    settings->reserved = 0;
    settings->roll_off = d.roll_off;
    return MS110D_OK;
}

ms110d_status ms110d_tx_memory_size(const ms110d_tx_settings* settings, uint32_t max_payload_bits, uint32_t* bytes,
                                    uint32_t* alignment) {
    if (bytes == nullptr || alignment == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    TxSettings s;
    ms110d_status st = settings_from_c(settings, s);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = block_bytes(s, max_payload_bits, total);
    if (st != MS110D_OK) return st;
    *bytes = static_cast<uint32_t>(total);
    *alignment = static_cast<uint32_t>(block_alignment);
    return MS110D_OK;
}

ms110d_status ms110d_tx_init(void* memory, uint32_t bytes, const ms110d_tx_settings* settings, uint32_t max_payload_bits,
                             ms110d_tx** tx) {
    if (tx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    TxSettings s;
    ms110d_status st = settings_from_c(settings, s);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = block_bytes(s, max_payload_bits, total);
    if (st != MS110D_OK) return st;
    if (memory == nullptr || bytes < total || reinterpret_cast<std::uintptr_t>(memory) % block_alignment != 0) {
        return MS110D_ERR_BAD_MEMORY;
    }
    return build_in(memory, bytes, s, max_payload_bits, tx);
}

ms110d_status ms110d_tx_create(const ms110d_tx_settings* settings, uint32_t max_payload_bits, ms110d_tx** tx) {
    if (tx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    TxSettings s;
    ms110d_status st = settings_from_c(settings, s);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = block_bytes(s, max_payload_bits, total);
    if (st != MS110D_OK) return st;
    void* memory = ::operator new(total, std::align_val_t{block_alignment}, std::nothrow);
    if (memory == nullptr) return MS110D_ERR_OUT_OF_MEMORY;
    ms110d_tx* made = nullptr;
    st = build_in(memory, total, s, max_payload_bits, &made);
    if (st != MS110D_OK) {
        ::operator delete(memory, std::align_val_t{block_alignment});
        return st;
    }
    *tx = made;
    return MS110D_OK;
}

void ms110d_tx_destroy(ms110d_tx* tx) {
    if (tx == nullptr) return;
    ::operator delete(static_cast<void*>(tx), std::align_val_t{block_alignment});
}

ms110d_status ms110d_tx_audio_length(const ms110d_tx* tx, uint32_t payload_bits, uint32_t* samples) {
    if (tx == nullptr || samples == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    std::size_t n = 0;
    const Status st = tx_of(tx)->audio_samples(payload_bits, n);
    if (st != Status::Ok) return to_c(st);
    if (n > std::numeric_limits<uint32_t>::max()) return MS110D_ERR_INVALID_ARGUMENT;
    *samples = static_cast<uint32_t>(n);
    return MS110D_OK;
}

ms110d_status ms110d_tx_begin(ms110d_tx* tx) {
    if (tx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    return to_c(tx_of(tx)->begin());
}

ms110d_status ms110d_tx_push_bits(ms110d_tx* tx, const uint8_t* bits, uint32_t count) {
    if (tx == nullptr || (bits == nullptr && count != 0)) return MS110D_ERR_INVALID_ARGUMENT;
    return to_c(tx_of(tx)->push_bits(std::span<const std::uint8_t>(bits, count)));
}

ms110d_status ms110d_tx_end_payload(ms110d_tx* tx) {
    if (tx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    return to_c(tx_of(tx)->end_payload());
}

ms110d_status ms110d_tx_modulate(ms110d_tx* tx, const uint8_t* bits, uint32_t bit_count, float* audio,
                                 uint32_t capacity, uint32_t* written) {
    if (written == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *written = 0;
    if (tx == nullptr || (bits == nullptr && bit_count != 0) || (audio == nullptr && capacity != 0)) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    std::size_t n = 0;
    const Status st = tx_of(tx)->modulate(std::span<const std::uint8_t>(bits, bit_count), std::span<float>(audio, capacity), n);
    if (n > std::numeric_limits<uint32_t>::max()) return MS110D_ERR_INVALID_ARGUMENT;
    *written = static_cast<uint32_t>(n);
    return to_c(st);
}

ms110d_status ms110d_tx_read(ms110d_tx* tx, float* audio, uint32_t capacity, uint32_t* written) {
    if (written == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *written = 0;
    if (tx == nullptr || audio == nullptr || capacity == 0) return MS110D_ERR_INVALID_ARGUMENT;
    std::size_t n = 0;
    const Status st = tx_of(tx)->read(std::span<float>(audio, capacity), n);
    *written = static_cast<uint32_t>(n);
    return to_c(st);
}

ms110d_status ms110d_tx_remaining(const ms110d_tx* tx, uint32_t* samples) {
    if (tx == nullptr || samples == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *samples = static_cast<uint32_t>(tx_of(tx)->remaining());
    return MS110D_OK;
}

}  // extern "C"
