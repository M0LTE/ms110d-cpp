// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <m0lte/ms110d/receiver.hpp>

#include "bcjr.hpp"
#include "dfe_receiver.hpp"
#include "fec/convolutional.hpp"
#include "fec/tail_biting_siso.hpp"
#include "fec/tail_biting_viterbi.hpp"
#include "interleaver.hpp"
#include "modulator.hpp"
#include "preamble_detector.hpp"
#include "rx_stream.hpp"
#include "tables.hpp"
#include "wn0_receiver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

// The public receiver: the shared front end (rx_stream.hpp) feeding the preamble detector, the WN 0
// receiver and the equaliser path, driven a T/2 sample at a time in the order of the C#
// Ms110dDemodulator.PushT2, with the C#'s burst handling around them (the lock, the terminate
// command, the WN 0 EOM scan and block limit, the return to the search). docs/dev/receiver.md.

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

constexpr int chips_superframe = 576;
// The longest WN 0 block (Table D-XXXVII, long): 1152 interleaver bits, 576 info bits.
constexpr std::size_t wn0_max_bits = 1152;
constexpr std::size_t max_events = 8;  // a sample raises at most four (two soft, block, end)
constexpr std::size_t block_alignment = 16;

Status validate(const RxOptions& o) noexcept {
    if (o.max_input_data_blocks < 0) return Status::InvalidArgument;
    if (!std::isfinite(o.sync_threshold)) return Status::InvalidArgument;
    if (o.rls_forgetting_factor && !std::isfinite(*o.rls_forgetting_factor)) return Status::InvalidArgument;
    if (o.track_ridge && !std::isfinite(*o.track_ridge)) return Status::InvalidArgument;
    const int pricing = static_cast<int>(o.turbo_noise_pricing);
    if (pricing < 0 || pricing > 2) return Status::InvalidArgument;
    const int il = static_cast<int>(o.max_interleaver);
    if (il < 0 || il > 3) return Status::InvalidArgument;
    if (!valid_roll_off(o.roll_off)) return Status::InvalidArgument;
    return Status::Ok;
}

DfeReceiverConfig dfe_config(const RxOptions& o) noexcept {
    DfeReceiverConfig c;
    c.roll_off = o.roll_off;
    c.max_interleaver = o.max_interleaver;
    c.max_input_data_blocks = o.max_input_data_blocks;
    c.turbo = !o.disable_turbo;
    c.has_rls_forgetting_factor = o.rls_forgetting_factor.has_value();
    c.rls_forgetting_factor = o.rls_forgetting_factor.value_or(0.0f);
    c.has_track_ridge = o.track_ridge.has_value();
    c.track_ridge = o.track_ridge.value_or(0.0f);
    c.noise_pricing = static_cast<DfeNoisePricing>(static_cast<int>(o.turbo_noise_pricing));
    c.presence_release_off = o.presence_release_off;
    return c;
}

std::size_t ring_for(InterleaverKind il) noexcept {
    return std::max(DfeReceiver::ring_size_for(il), PreambleDetector::ring_size);
}

std::size_t info_bits_for(InterleaverKind il) noexcept {
    return std::max(DfeReceiver::max_input_bits_for(il), wn0_max_bits / 2);
}

// The longest block (info bits) and interleaver (soft values) of any mode with an interleaver no
// longer than `il`.
void longest_block(InterleaverKind il, std::size_t& info, std::size_t& soft) noexcept {
    info = 0;
    soft = 0;
    for (const Mode& m : modes_3k()) {
        for (int kind = 0; kind <= static_cast<int>(il); ++kind) {
            const auto p = interleaver_3k(m.wn, static_cast<InterleaverKind>(kind));
            if (!p) continue;
            info = std::max(info, static_cast<std::size_t>(p->input_bits));
            soft = std::max(soft, static_cast<std::size_t>(p->size_bits));
        }
    }
}

std::size_t align_up(std::size_t n, std::size_t a) noexcept { return (n + a - 1) / a * a; }

}  // namespace

struct Receiver::Impl {
    enum class Phase { Searching, Locked, Wn0, Dfe };

    // Where each part goes in the caller's block.
    struct Layout {
        std::size_t impl = 0, ring = 0, dfe = 0, viterbi = 0, siso = 0, chain = 0, total = 0;
        std::size_t ring_n = 0, dfe_bytes = 0, viterbi_bytes = 0, siso_bytes = 0, chain_bytes = 0;
    };
    static Layout layout(const RxOptions& o) noexcept;

    struct Queued {
        RxEvent event;
    };

    RxOptions options{};
    RxStream stream{};
    PreambleDetector detector{};
    Wn0Receiver wn0{};
    DfeReceiver dfe{};
    fec::TailBitingViterbiDecoder viterbi{};
    fec::TailBitingSisoDecoder siso{};
    BasicChainBcjr<float> chain{};
    std::span<std::uint8_t> viterbi_storage{};
    std::span<std::uint8_t> siso_storage{};
    std::size_t max_info_bits = 0;

    // WN 0 block buffers.
    std::array<real, wn0_max_bits> wn0_llrs{};
    std::array<float, wn0_max_bits> wn0_soft{};
    std::array<std::uint8_t, wn0_max_bits / 2> wn0_info{};

    // The burst.
    Phase phase = Phase::Searching;
    std::int64_t locks_seen = 0;
    int waveform = -1;
    InterleaverKind interleaver = InterleaverKind::Short;
    int constraint_length = 0;
    double cfo_hz = 0;
    // WN 0 block and burst assembly (the equaliser path keeps its own).
    int block_index = 0;
    std::int64_t burst_bits = 0;
    std::array<std::uint8_t, 31> eom_tail{};
    int eom_tail_len = 0;
    bool terminate_pending = false;
    std::int64_t refused = 0;
    std::size_t max_block_bits = 0;
    std::size_t max_soft_values = 0;
    std::int64_t wn0_turbo_skipped = 0;

    // Events.
    std::array<Queued, max_events> queue{};
    std::size_t head = 0;
    std::size_t count = 0;

    RxEvent make(RxEventKind kind) const noexcept {
        RxEvent e;
        e.kind = kind;
        e.sample = stream.used();
        e.waveform = waveform;
        e.interleaver = interleaver;
        e.constraint_length = constraint_length;
        e.cfo_hz = cfo_hz;
        return e;
    }

    void push(const RxEvent& e) noexcept {
        if (count == max_events) return;  // cannot happen: a sample raises at most four
        queue[(head + count) % max_events].event = e;
        ++count;
    }

    void reset() noexcept {
        stream.reset();
        detector.reset();
        wn0.reset();
        dfe.reset();
        phase = Phase::Searching;
        locks_seen = 0;
        waveform = -1;
        constraint_length = 0;
        cfo_hz = 0;
        terminate_pending = false;
        refused = 0;
        wn0_turbo_skipped = 0;
        head = 0;
        count = 0;
    }

    // The burst ended: the event, and back to the search (the C# EndBurst and BackToSearch).
    void end_burst(BurstEndReason reason, int blocks, std::int64_t payload_bits) noexcept {
        RxEvent e = make(RxEventKind::BurstCompleted);
        e.reason = reason;
        e.blocks = blocks;
        e.payload_bits = payload_bits;
        push(e);
        phase = Phase::Searching;
        detector.end_burst(reason == BurstEndReason::Terminated);
    }

    // The terminate command, at the top of a T/2 sample as the C# PushT2. True if a burst ended,
    // in which case the C# does nothing else with this sample.
    bool apply_terminate() noexcept {
        switch (phase) {
            case Phase::Wn0:
                wn0.stop();
                end_burst(BurstEndReason::Terminated, block_index, burst_bits);
                return true;
            case Phase::Dfe:
                dfe.terminate();
                collect_dfe();
                return true;
            case Phase::Locked:  // the C# is already Tracking here, before its first data
                end_burst(BurstEndReason::Terminated, 0, 0);
                return true;
            case Phase::Searching:
                if (!detector.searching()) detector.resume_search();  // reading a preamble: back to the search
                return false;
        }
        return false;
    }

    void on_lock() noexcept {
        const PreambleDetection& d = detector.lock_info();
        waveform = d.waveform;
        interleaver = d.interleaver;
        constraint_length = d.constraint_length;
        cfo_hz = d.cfo_hz_first_fit;
        // One rule for every waveform, WN 0 included (its own buffers would take any interleaver,
        // but max_interleaver means the same for every mode).
        if (static_cast<int>(d.interleaver) > static_cast<int>(options.max_interleaver)) {
            ++refused;
            RxEvent r = make(RxEventKind::BurstRefused);
            r.sample = d.locked_at_sample;
            push(r);
            detector.resume_search();
            return;
        }
        RxEvent e = make(RxEventKind::BurstStarted);
        e.sample = d.locked_at_sample;
        push(e);
        phase = Phase::Locked;
    }

    void on_detection() noexcept {
        PreambleDetection d;
        detector.take_detection(d);
        cfo_hz = d.cfo_hz;
        const fec::ConvolutionalCode& code = d.constraint_length == 9 ? fec::k9_561_753 : fec::k7_133_171;
        // One Viterbi and one SISO, set up for this burst's code in storage sized for K = 9.
        bool ok = viterbi.configure(code, max_info_bits, viterbi_storage) && siso.configure(code, max_info_bits, siso_storage);
        if (ok && d.waveform == 0) {
            const auto il = interleaver_3k(0, d.interleaver);
            ok = il.has_value();
            if (ok) {
                Wn0Start s;
                s.chip0 = d.chip0_t2;
                s.tau = 0;
                s.omega = d.carrier_omega;
                s.theta_base = d.carrier_theta_base;
                s.data_start_chip = static_cast<std::int64_t>(chips_superframe) * (d.superframes_after + 1);
                s.interleaver = Interleaver(static_cast<std::uint32_t>(il->size_bits), static_cast<std::uint32_t>(il->increment));
                ok = wn0.start(s, wn0_llrs);
            }
            if (ok) {
                block_index = 0;
                burst_bits = 0;
                eom_tail_len = 0;
                phase = Phase::Wn0;
                return;
            }
        } else if (ok) {
            ok = dfe.start(dfe_start_from_detection(d));
            if (ok) {
                phase = Phase::Dfe;
                return;
            }
        }
        // Not expected (the lock was checked): treat as refused.
        ++refused;
        phase = Phase::Searching;
        detector.resume_search();
    }

    void collect_dfe() noexcept {
        while (dfe.event() != DfeEvent::None) {
            if (dfe.event() == DfeEvent::BlockDecoded) {
                const DfeBlock& b = dfe.block();
                if (options.soft_decisions) {
                    RxEvent s = make(RxEventKind::FirstPassSoft);
                    s.block_index = b.index;
                    s.soft = b.llrs;
                    push(s);
                    if (!b.turbo_llrs.empty()) {
                        RxEvent t = make(RxEventKind::TurboSoft);
                        t.block_index = b.index;
                        t.soft = b.turbo_llrs;
                        push(t);
                    }
                }
                RxEvent e = make(RxEventKind::BlockDecoded);
                e.block_index = b.index;
                e.bits = b.info;
                push(e);
            } else {
                const DfeBurstEnd& end = dfe.burst_end();
                BurstEndReason r = BurstEndReason::SignalLost;
                switch (end.reason) {
                    case DfeEndReason::Eom: r = BurstEndReason::Eom; break;
                    case DfeEndReason::MaxInputDataBlocks: r = BurstEndReason::MaxInputDataBlocks; break;
                    case DfeEndReason::SignalLost: r = BurstEndReason::SignalLost; break;
                    case DfeEndReason::SignalAbsent: r = BurstEndReason::SignalAbsent; break;
                    case DfeEndReason::Terminated: r = BurstEndReason::Terminated; break;
                    case DfeEndReason::NewPreamble: r = BurstEndReason::NewPreamble; break;
                }
                end_burst(r, end.blocks, end.payload_bits);
            }
            dfe.take_event();
        }
    }

    // The WN 0 block end of the C# FinishBlock: the soft values, the Viterbi decode, the EOM scan
    // over the burst's bits from 31 before this block, and the block limit.
    void finish_wn0_block() noexcept {
        const std::size_t size = wn0.block_llrs().size();
        const std::size_t input = size / 2;
        if (options.soft_decisions) {
            for (std::size_t i = 0; i < size; ++i) wn0_soft[i] = static_cast<float>(wn0.block_llrs()[i]);
            RxEvent s = make(RxEventKind::FirstPassSoft);
            s.block_index = block_index;
            s.soft = {wn0_soft.data(), size};
            push(s);
        }
        const std::span<std::uint8_t> info{wn0_info.data(), input};
        if (!wn0.decode_block(viterbi, info)) std::fill(info.begin(), info.end(), std::uint8_t{0});
        ++wn0_turbo_skipped;  // the C# counts every WN 0 block as skipped by the turbo stage

        const std::int64_t before = burst_bits;
        const std::int64_t search_from = std::max<std::int64_t>(0, before - 31);
        const std::int64_t total = before + static_cast<std::int64_t>(input);
        const auto bit_at = [&](std::int64_t i) -> std::uint8_t {
            const std::int64_t back = before - i;
            return back > 0 ? eom_tail[static_cast<std::size_t>(eom_tail_len - back)] : info[static_cast<std::size_t>(i - before)];
        };
        std::uint32_t window = 0;
        std::int64_t eom = -1;
        for (std::int64_t i = search_from; i < total; ++i) {
            window = (window << 1) | bit_at(i);
            if (i - search_from >= 31 && window == tables::eom) {
                eom = i - 31;
                break;
            }
        }
        std::array<std::uint8_t, 31> tail{};
        int len = 0;
        for (std::int64_t i = std::max<std::int64_t>(0, total - 31); i < total; ++i) tail[static_cast<std::size_t>(len++)] = bit_at(i);
        eom_tail = tail;
        eom_tail_len = len;
        burst_bits = total;

        RxEvent e = make(RxEventKind::BlockDecoded);
        e.block_index = block_index;
        e.bits = info;
        push(e);
        ++block_index;

        if (eom >= 0) {
            wn0.stop();
            end_burst(BurstEndReason::Eom, block_index, eom);
        } else if (options.max_input_data_blocks > 0 && block_index >= options.max_input_data_blocks) {
            wn0.stop();
            end_burst(BurstEndReason::MaxInputDataBlocks, block_index, burst_bits);
        }
    }

    void track_wn0() noexcept {
        for (;;) {
            const Wn0Step r = wn0.step();
            if (r == Wn0Step::Symbol) continue;
            if (r == Wn0Step::BlockComplete) {
                finish_wn0_block();
                if (phase != Phase::Wn0) return;
                continue;
            }
            if (r == Wn0Step::SignalLost) end_burst(BurstEndReason::SignalLost, block_index, burst_bits);
            return;
        }
    }

    // The background search (the C# ShadowStep, C# issue 10): true if a new preamble passed every gate
    // and the burst should be let go of. Not for a preamble whose interleaver is longer than
    // max_interleaver: this receiver would refuse that burst, so dropping the one it is decoding
    // for it would lose both (the C# has no max_interleaver; with the default nothing is refused).
    bool background_new_preamble() noexcept {
        if (!detector.background_step()) return false;
        if (static_cast<int>(detector.background_interleaver()) > static_cast<int>(options.max_interleaver)) {
            detector.reject_background();
            return false;
        }
        return true;
    }

    // One new T/2 sample, in the order of the C# PushT2: the terminate command, then the stage.
    void on_t2() noexcept {
        if (terminate_pending) {
            terminate_pending = false;
            if (apply_terminate()) {
                detector.on_t2(false);  // the running energies only, as the C# returns here
                return;
            }
        }
        detector.on_t2(true);
        switch (phase) {
            case Phase::Searching:
                if (detector.stats().locks != locks_seen) {
                    locks_seen = detector.stats().locks;
                    on_lock();
                }
                if (phase == Phase::Locked && detector.has_detection()) on_detection();
                break;
            case Phase::Locked:
                if (detector.has_detection()) on_detection();
                break;
            case Phase::Wn0:
                track_wn0();
                if (phase == Phase::Wn0 && background_new_preamble()) {
                    wn0.stop();
                    end_burst(BurstEndReason::NewPreamble, block_index, burst_bits);
                }
                break;
            case Phase::Dfe:
                dfe.on_t2();
                collect_dfe();
                if (phase == Phase::Dfe && background_new_preamble()) {
                    dfe.release_for_new_preamble();
                    collect_dfe();
                }
                break;
        }
    }
};

Receiver::Impl::Layout Receiver::Impl::layout(const RxOptions& o) noexcept {
    Layout l;
    const std::size_t info = info_bits_for(o.max_interleaver);
    l.ring_n = ring_for(o.max_interleaver);
    l.dfe_bytes = DfeReceiver::storage_bytes(dfe_config(o));
    l.viterbi_bytes = fec::TailBitingViterbiDecoder::storage_bytes(fec::k9_561_753, info);
    l.siso_bytes = fec::TailBitingSisoDecoder::storage_bytes(fec::k9_561_753, info);
    l.chain_bytes = BasicChainBcjr<float>::storage_bytes(DfeReceiver::max_frame_symbols, 16);
    std::size_t at = 0;
    l.impl = at;
    at = align_up(at + sizeof(Impl), block_alignment);
    l.ring = at;
    at = align_up(at + l.ring_n * sizeof(dsp::ComplexF), block_alignment);
    l.dfe = at;
    at = align_up(at + l.dfe_bytes, block_alignment);
    l.viterbi = at;
    at = align_up(at + l.viterbi_bytes, block_alignment);
    l.siso = at;
    at = align_up(at + l.siso_bytes, block_alignment);
    l.chain = at;
    at = align_up(at + l.chain_bytes, block_alignment);
    l.total = at;
    return l;
}

Status Receiver::storage_bytes(const RxOptions& options, std::size_t& bytes) noexcept {
    const Status st = validate(options);
    if (st != Status::Ok) return st;
    bytes = Impl::layout(options).total + block_alignment;  // room to align the start
    return Status::Ok;
}

Status Receiver::configure(const RxOptions& options, std::span<std::uint8_t> storage) noexcept {
    std::size_t need = 0;
    const Status st = storage_bytes(options, need);
    if (st != Status::Ok) return st;
    if (storage.size() < need) return Status::BufferTooSmall;
    const Impl::Layout l = Impl::layout(options);
    void* where = storage.data();
    std::size_t space = storage.size();
    if (std::align(block_alignment, l.total, where, space) == nullptr) return Status::BufferTooSmall;
    auto* base = static_cast<std::uint8_t*>(where);

    // The options are valid, so nothing below can fail; check anyway, before taking over.
    Impl* impl = ::new (base + l.impl) Impl();
    impl->options = options;
    impl->max_info_bits = info_bits_for(options.max_interleaver);
    longest_block(options.max_interleaver, impl->max_block_bits, impl->max_soft_values);
    impl->viterbi_storage = {base + l.viterbi, l.viterbi_bytes};
    impl->siso_storage = {base + l.siso, l.siso_bytes};
    bool ok = impl->stream.configure(options.roll_off, {reinterpret_cast<dsp::ComplexF*>(base + l.ring), l.ring_n});
    PreambleDetectorConfig dc;
    dc.roll_off = options.roll_off;
    dc.sync_threshold = options.sync_threshold;
    ok = ok && impl->detector.configure(dc, impl->stream);
    ok = ok && impl->wn0.configure(impl->stream);
    ok = ok && impl->viterbi.configure(fec::k9_561_753, impl->max_info_bits, impl->viterbi_storage);
    ok = ok && impl->siso.configure(fec::k9_561_753, impl->max_info_bits, impl->siso_storage);
    ok = ok && impl->chain.configure(DfeReceiver::max_frame_symbols, 16, {base + l.chain, l.chain_bytes});
    DfeDecoders d;
    d.viterbi_k7 = d.viterbi_k9 = &impl->viterbi;
    d.siso_k7 = d.siso_k9 = &impl->siso;
    d.chain = &impl->chain;
    ok = ok && impl->dfe.configure(dfe_config(options), {base + l.dfe, l.dfe_bytes}, d, impl->stream);
    if (!ok) {
        impl->~Impl();
        return Status::InvalidArgument;
    }
    impl->reset();
    impl_ = impl;
    return Status::Ok;
}

Status Receiver::process(std::span<const float> audio, std::size_t& consumed) noexcept {
    consumed = 0;
    if (impl_ == nullptr || impl_->count != 0) return Status::State;
    Impl& m = *impl_;
    for (const float sample : audio) {
        ++consumed;
        if (!m.stream.push(sample)) continue;
        m.on_t2();
        if (m.count != 0) break;
    }
    return Status::Ok;
}

RxEvent Receiver::next_event() noexcept {
    if (impl_ == nullptr || impl_->count == 0) return RxEvent{};
    const RxEvent e = impl_->queue[impl_->head].event;
    impl_->head = (impl_->head + 1) % max_events;
    --impl_->count;
    return e;
}

RxEvent Receiver::peek_event() const noexcept {
    if (impl_ == nullptr || impl_->count == 0) return RxEvent{};
    return impl_->queue[impl_->head].event;
}

void Receiver::terminate() noexcept {
    if (impl_ != nullptr) impl_->terminate_pending = true;
}

void Receiver::reset() noexcept {
    if (impl_ != nullptr) impl_->reset();
}

RxState Receiver::state() const noexcept {
    if (impl_ == nullptr) return RxState::Searching;
    switch (impl_->phase) {
        case Impl::Phase::Searching: return impl_->detector.searching() ? RxState::Searching : RxState::ReadingPreamble;
        default: return RxState::Tracking;
    }
}

RxStats Receiver::stats() const noexcept {
    RxStats s;
    if (impl_ == nullptr) return s;
    const DfeReceiverStats& d = impl_->dfe.stats();
    s.peak_search_metric = impl_->detector.stats().peak_search_metric;
    s.turbo_converged = d.turbo_converged;
    s.turbo_reverted = d.turbo_reverted;
    s.turbo_salvaged = d.turbo_salvaged;
    s.turbo_aborted = d.turbo_aborted;
    s.turbo_skipped = d.turbo_skipped + impl_->wn0_turbo_skipped;
    s.collapse_resolves = d.collapse_resolves;
    s.signal_absent_releases = d.signal_absent_releases;
    s.mfb_offered = d.mfb_offered;
    s.mfb_selected = d.mfb_selected;
    s.agc_resolves = d.agc_resolves;
    s.bursts_refused = impl_->refused;
    s.samples_taken = impl_->stream.used();
    return s;
}

std::int64_t Receiver::samples_taken() const noexcept { return impl_ == nullptr ? 0 : impl_->stream.used(); }

std::size_t Receiver::max_block_bits() const noexcept { return impl_ == nullptr ? 0 : impl_->max_block_bits; }

std::size_t Receiver::max_soft_values() const noexcept { return impl_ == nullptr ? 0 : impl_->max_soft_values; }

}  // inline namespace

}  // namespace m0lte::ms110d
