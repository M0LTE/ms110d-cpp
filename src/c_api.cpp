// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <m0lte/ms110d/ms110d.h>

#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/receiver.hpp>
#include <m0lte/ms110d/transmitter.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
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

// ---- receive

using m0lte::ms110d::Receiver;
using m0lte::ms110d::RxEvent;
using m0lte::ms110d::RxEventKind;
using m0lte::ms110d::RxOptions;

constexpr std::size_t rx_header_bytes = (sizeof(Receiver) + block_alignment - 1) / block_alignment * block_alignment;

// The struct_size policy for an input struct, as settings_from_c.
bool struct_size_ok(const void* in, std::uint32_t struct_size, std::size_t known) noexcept {
    if (struct_size < known) return false;
    const auto* raw = static_cast<const std::uint8_t*>(in);
    for (std::size_t i = known; i < struct_size; ++i) {
        if (raw[i] != 0) return false;
    }
    return true;
}

ms110d_status options_from_c(const ms110d_rx_options* in, RxOptions& out) noexcept {
    if (in == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    if (!struct_size_ok(in, in->struct_size, sizeof(ms110d_rx_options)) || in->reserved != 0) return MS110D_ERR_INVALID_ARGUMENT;
    const auto flag = [](std::int32_t v) { return v == 0 || v == 1; };
    if (!flag(in->disable_turbo) || !flag(in->presence_release_off) || !flag(in->has_rls_forgetting_factor) ||
        !flag(in->has_track_ridge) || !flag(in->soft_decisions)) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    if (in->max_interleaver < MS110D_INTERLEAVER_ULTRA_SHORT || in->max_interleaver > MS110D_INTERLEAVER_LONG) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    if (in->turbo_noise_pricing < MS110D_NOISE_PRICING_SPIKE_UP || in->turbo_noise_pricing > MS110D_NOISE_PRICING_SPIKE_2S) {
        return MS110D_ERR_INVALID_ARGUMENT;
    }
    out.max_input_data_blocks = in->max_input_data_blocks;
    out.disable_turbo = in->disable_turbo != 0;
    out.presence_release_off = in->presence_release_off != 0;
    out.turbo_noise_pricing = static_cast<m0lte::ms110d::TurboNoisePricing>(in->turbo_noise_pricing);
    out.rls_forgetting_factor.reset();
    if (in->has_rls_forgetting_factor != 0) out.rls_forgetting_factor = in->rls_forgetting_factor;
    out.track_ridge.reset();
    if (in->has_track_ridge != 0) out.track_ridge = in->track_ridge;
    out.max_interleaver = static_cast<m0lte::ms110d::InterleaverKind>(in->max_interleaver);
    out.soft_decisions = in->soft_decisions != 0;
    out.sync_threshold = in->sync_threshold;
    out.roll_off = in->roll_off;
    return MS110D_OK;
}

ms110d_status rx_block_bytes(const RxOptions& o, std::size_t& total) noexcept {
    std::size_t storage = 0;
    const Status st = Receiver::storage_bytes(o, storage);
    if (st != Status::Ok) return to_c(st);
    total = rx_header_bytes + storage;
    if (total > std::numeric_limits<uint32_t>::max()) return MS110D_ERR_UNSUPPORTED;
    return MS110D_OK;
}

ms110d_status rx_build_in(void* memory, std::size_t bytes, const RxOptions& o, ms110d_rx** out) noexcept {
    auto* rx = ::new (memory) Receiver();
    const Status st = rx->configure(o, std::span<std::uint8_t>(static_cast<std::uint8_t*>(memory) + rx_header_bytes, bytes - rx_header_bytes));
    if (st != Status::Ok) {
        rx->~Receiver();
        return to_c(st);
    }
    *out = reinterpret_cast<ms110d_rx*>(rx);
    return MS110D_OK;
}

Receiver* rx_of(ms110d_rx* h) noexcept { return reinterpret_cast<Receiver*>(h); }
const Receiver* rx_of(const ms110d_rx* h) noexcept { return reinterpret_cast<const Receiver*>(h); }

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

ms110d_status ms110d_rx_options_default(ms110d_rx_options* options) {
    if (options == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    const RxOptions d;
    std::memset(options, 0, sizeof(ms110d_rx_options));
    options->struct_size = sizeof(ms110d_rx_options);
    options->max_input_data_blocks = d.max_input_data_blocks;
    options->disable_turbo = d.disable_turbo ? 1 : 0;
    options->presence_release_off = d.presence_release_off ? 1 : 0;
    options->turbo_noise_pricing = static_cast<int32_t>(d.turbo_noise_pricing);
    options->has_rls_forgetting_factor = d.rls_forgetting_factor ? 1 : 0;
    options->rls_forgetting_factor = d.rls_forgetting_factor.value_or(0.0f);
    options->has_track_ridge = d.track_ridge ? 1 : 0;
    options->track_ridge = d.track_ridge.value_or(0.0f);
    options->max_interleaver = static_cast<int32_t>(d.max_interleaver);
    options->soft_decisions = d.soft_decisions ? 1 : 0;
    options->reserved = 0;
    options->sync_threshold = d.sync_threshold;
    options->roll_off = d.roll_off;
    return MS110D_OK;
}

ms110d_status ms110d_rx_memory_size(const ms110d_rx_options* options, uint32_t* bytes, uint32_t* alignment) {
    if (bytes == nullptr || alignment == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    RxOptions o;
    ms110d_status st = options_from_c(options, o);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = rx_block_bytes(o, total);
    if (st != MS110D_OK) return st;
    *bytes = static_cast<uint32_t>(total);
    *alignment = static_cast<uint32_t>(block_alignment);
    return MS110D_OK;
}

ms110d_status ms110d_rx_init(void* memory, uint32_t bytes, const ms110d_rx_options* options, ms110d_rx** rx) {
    if (rx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    RxOptions o;
    ms110d_status st = options_from_c(options, o);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = rx_block_bytes(o, total);
    if (st != MS110D_OK) return st;
    if (memory == nullptr || bytes < total || reinterpret_cast<std::uintptr_t>(memory) % block_alignment != 0) {
        return MS110D_ERR_BAD_MEMORY;
    }
    return rx_build_in(memory, bytes, o, rx);
}

ms110d_status ms110d_rx_create(const ms110d_rx_options* options, ms110d_rx** rx) {
    if (rx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    RxOptions o;
    ms110d_status st = options_from_c(options, o);
    if (st != MS110D_OK) return st;
    std::size_t total = 0;
    st = rx_block_bytes(o, total);
    if (st != MS110D_OK) return st;
    void* memory = ::operator new(total, std::align_val_t{block_alignment}, std::nothrow);
    if (memory == nullptr) return MS110D_ERR_OUT_OF_MEMORY;
    ms110d_rx* made = nullptr;
    st = rx_build_in(memory, total, o, &made);
    if (st != MS110D_OK) {
        ::operator delete(memory, std::align_val_t{block_alignment});
        return st;
    }
    *rx = made;
    return MS110D_OK;
}

void ms110d_rx_destroy(ms110d_rx* rx) {
    if (rx == nullptr) return;
    rx_of(rx)->~Receiver();
    ::operator delete(static_cast<void*>(rx), std::align_val_t{block_alignment});
}

ms110d_status ms110d_rx_process(ms110d_rx* rx, const float* samples, uint32_t count, uint32_t* consumed) {
    if (consumed == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *consumed = 0;
    if (rx == nullptr || (samples == nullptr && count != 0)) return MS110D_ERR_INVALID_ARGUMENT;
    std::size_t used = 0;
    const Status st = rx_of(rx)->process(std::span<const float>(samples, count), used);
    *consumed = static_cast<uint32_t>(used);
    return to_c(st);
}

ms110d_status ms110d_rx_next_event(ms110d_rx* rx, ms110d_rx_event* event, uint8_t* bits, uint32_t bits_capacity, float* soft,
                                   uint32_t soft_capacity) {
    if (rx == nullptr || event == nullptr || event->struct_size < sizeof(ms110d_rx_event)) return MS110D_ERR_INVALID_ARGUMENT;
    if ((bits == nullptr && bits_capacity != 0) || (soft == nullptr && soft_capacity != 0)) return MS110D_ERR_INVALID_ARGUMENT;
    const RxEvent e = rx_of(rx)->peek_event();
    // Only this library's fields are written; any bytes of a newer caller's larger struct are zeroed.
    const uint32_t size = event->struct_size;
    std::memset(reinterpret_cast<std::uint8_t*>(event) + sizeof(std::uint32_t), 0, size - sizeof(std::uint32_t));
    event->kind = static_cast<uint32_t>(e.kind);
    event->sample = e.sample;
    event->waveform_number = e.waveform;
    event->interleaver = static_cast<int32_t>(e.interleaver);
    event->constraint_length = e.constraint_length;
    event->block_index = e.block_index;
    event->end_reason = static_cast<int32_t>(e.reason);
    event->blocks = e.blocks;
    event->payload_bits = e.payload_bits;
    event->cfo_hz = e.cfo_hz;
    if (e.kind == RxEventKind::None) return MS110D_OK;
    if (e.kind == RxEventKind::BlockDecoded) {
        event->length = static_cast<uint32_t>(e.bits.size());
        if (bits_capacity < e.bits.size()) return MS110D_ERR_BUFFER_TOO_SMALL;
        if (!e.bits.empty()) std::memcpy(bits, e.bits.data(), e.bits.size());
    } else if (e.kind == RxEventKind::FirstPassSoft || e.kind == RxEventKind::TurboSoft) {
        event->length = static_cast<uint32_t>(e.soft.size());
        if (soft_capacity < e.soft.size()) return MS110D_ERR_BUFFER_TOO_SMALL;
        if (!e.soft.empty()) std::memcpy(soft, e.soft.data(), e.soft.size() * sizeof(float));
    }
    (void)rx_of(rx)->next_event();
    return MS110D_OK;
}

ms110d_status ms110d_rx_drop_event(ms110d_rx* rx) {
    if (rx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    (void)rx_of(rx)->next_event();
    return MS110D_OK;
}

ms110d_status ms110d_rx_buffer_sizes(const ms110d_rx* rx, uint32_t* bits, uint32_t* soft_values) {
    if (rx == nullptr || bits == nullptr || soft_values == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *bits = static_cast<uint32_t>(rx_of(rx)->max_block_bits());
    *soft_values = static_cast<uint32_t>(rx_of(rx)->max_soft_values());
    return MS110D_OK;
}

ms110d_status ms110d_rx_get_stats(const ms110d_rx* rx, ms110d_rx_stats* stats) {
    if (rx == nullptr || stats == nullptr || stats->struct_size < sizeof(ms110d_rx_stats)) return MS110D_ERR_INVALID_ARGUMENT;
    const m0lte::ms110d::RxStats s = rx_of(rx)->stats();
    const uint32_t size = stats->struct_size;
    std::memset(reinterpret_cast<std::uint8_t*>(stats) + sizeof(std::uint32_t), 0, size - sizeof(std::uint32_t));
    stats->peak_search_metric = s.peak_search_metric;
    stats->samples_taken = s.samples_taken;
    stats->turbo_converged = s.turbo_converged;
    stats->turbo_reverted = s.turbo_reverted;
    stats->turbo_salvaged = s.turbo_salvaged;
    stats->turbo_aborted = s.turbo_aborted;
    stats->turbo_skipped = s.turbo_skipped;
    stats->collapse_resolves = s.collapse_resolves;
    stats->signal_absent_releases = s.signal_absent_releases;
    stats->agc_resolves = s.agc_resolves;
    stats->mfb_offered = s.mfb_offered;
    stats->mfb_selected = s.mfb_selected;
    stats->bursts_refused = s.bursts_refused;
    return MS110D_OK;
}

ms110d_status ms110d_rx_terminate(ms110d_rx* rx) {
    if (rx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    rx_of(rx)->terminate();
    return MS110D_OK;
}

ms110d_status ms110d_rx_reset(ms110d_rx* rx) {
    if (rx == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    rx_of(rx)->reset();
    return MS110D_OK;
}

ms110d_status ms110d_rx_state(const ms110d_rx* rx, int32_t* state) {
    if (rx == nullptr || state == nullptr) return MS110D_ERR_INVALID_ARGUMENT;
    *state = static_cast<int32_t>(rx_of(rx)->state());
    return MS110D_OK;
}

}  // extern "C"
