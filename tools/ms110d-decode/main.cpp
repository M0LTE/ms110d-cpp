// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// ms110d-decode: runs the public C++ Receiver over a WAV file and prints one JSON object per line
// per event, for the shadow receiver (docs/dev/shadow.md). The input is mono 16-bit PCM or 32-bit
// float at any rate of 9600 Hz or more; other rates are resampled to 9600 Hz the way
// scripts/corpus/curate.py does. The audio is fed in chunks of 96 samples, followed by three
// seconds of silence and a terminate if a burst is still open, exactly as the corpus tools feed
// the C# receiver (tools/csharp-vectors/CorpusDecode.cs).
//
// Events (all have "ev" and "decoder":"cxx"):
//   burst_started    burst, sample, wn, il, k, cfo_hz
//   burst_refused    burst, sample, wn, il, k   (only with --max-interleaver below Long)
//   block            burst, index, sample, bits, bits_hex (packed MSB first, zero padded)
//   burst_completed  burst, sample, start_sample, wn, il, k, blocks, reason, payload_bits, snr_db, cpu_ms
//   summary          version, audio_s, bursts, cpu_ms_total, cpu_ms_in_bursts, cpu_ms_search
// The block bits stay in the output so a second pass (the C# twin, `shadow finish`) can run the
// IL2P deframer, which only exists in C#, and add the payload hash and the good frame counts.

#include <m0lte/ms110d/receiver.hpp>
#include <m0lte/ms110d/version.hpp>

#include "resample.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using namespace m0lte::ms110d;

const char* il_name(InterleaverKind k) {
    switch (k) {
        case InterleaverKind::UltraShort: return "UltraShort";
        case InterleaverKind::Short: return "Short";
        case InterleaverKind::Medium: return "Medium";
        case InterleaverKind::Long: return "Long";
    }
    return "Unknown";
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
    return "Unknown";
}

// "<version>+<git commit>" (the commit the tool was configured from, "unknown" outside a checkout).
std::string version_text() {
    Version v = version();
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch) + "+" + MS110D_TOOL_GIT;
}

// The slot's quiet floor: the median power of the quietest fifth of its 0.5 s windows, as
// scripts/corpus/curate.py computes it (docs/dev/corpus.md). The receivers expose no SNR of their own, so the
// shadow report measures one from the audio, the same way in both tools.
double noise_floor_power(const std::vector<float>& x) {
    const size_t n = 4800, k = x.size() / n;
    if (k == 0) return 1e-18;
    std::vector<double> p(k);
    for (size_t w = 0; w < k; ++w) {
        double acc = 0;
        for (size_t i = 0; i < n; ++i) acc += static_cast<double>(x[w * n + i]) * static_cast<double>(x[w * n + i]);
        p[w] = acc / static_cast<double>(n);
    }
    std::sort(p.begin(), p.end());
    const size_t m = std::max<size_t>(1, k / 5);
    const double med = (m % 2 == 1) ? p[m / 2] : 0.5 * (p[m / 2 - 1] + p[m / 2]);
    return med + 1e-18;
}

double cpu_ms() { return 1000.0 * static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }

void write_wav16(const std::string& path, const std::vector<std::int16_t>& s) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    auto w32 = [&](std::uint32_t v) {
        unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto w16 = [&](std::uint16_t v) {
        unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    auto bytes = static_cast<std::uint32_t>(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(1); w32(9600); w32(19200); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    for (std::int16_t v : s) w16(static_cast<std::uint16_t>(v));
    std::fclose(f);
}

struct Tool {
    Receiver rx;
    std::vector<std::uint8_t> memory;
    int burst = -1;            // index of the current or last burst
    std::int64_t started_at = 0;
    double burst_cpu = 0, cpu_in_bursts = 0, cpu_search = 0;
    int bursts = 0;
    const std::vector<float>* audio = nullptr;
    double floor_power = 1e-18;

    // The burst's power over the slot's floor, in dB, from the lock to the end of the burst (curate.py's estimate).
    double snr_db(std::int64_t from, std::int64_t to) const {
        to = std::min<std::int64_t>(to, static_cast<std::int64_t>(audio->size()));
        if (to <= from) return -20.0;
        double acc = 0;
        for (std::int64_t i = from; i < to; ++i) acc += static_cast<double>((*audio)[static_cast<size_t>(i)]) * static_cast<double>((*audio)[static_cast<size_t>(i)]);
        const double sig = acc / static_cast<double>(to - from);
        if (sig <= floor_power) return -20.0;
        return 10.0 * std::log10(std::max(sig - floor_power, 1e-18) / floor_power);
    }

    void drain() {
        for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
            switch (e.kind) {
                case RxEventKind::BurstStarted:
                    ++burst;
                    ++bursts;
                    started_at = e.sample;
                    std::printf("{\"ev\":\"burst_started\",\"decoder\":\"cxx\",\"burst\":%d,\"sample\":%lld,\"wn\":%d,"
                                "\"il\":\"%s\",\"k\":%d,\"cfo_hz\":%.3f}\n",
                                burst, static_cast<long long>(e.sample), e.waveform, il_name(e.interleaver),
                                e.constraint_length, e.cfo_hz);
                    break;
                case RxEventKind::BurstRefused:
                    ++burst;
                    std::printf("{\"ev\":\"burst_refused\",\"decoder\":\"cxx\",\"burst\":%d,\"sample\":%lld,\"wn\":%d,"
                                "\"il\":\"%s\",\"k\":%d}\n",
                                burst, static_cast<long long>(e.sample), e.waveform, il_name(e.interleaver),
                                e.constraint_length);
                    break;
                case RxEventKind::BlockDecoded: {
                    std::string hex;
                    hex.reserve((e.bits.size() + 7) / 8 * 2);
                    for (size_t i = 0; i < e.bits.size(); i += 8) {
                        unsigned v = 0;
                        for (size_t j = 0; j < 8; ++j) v = (v << 1) | (i + j < e.bits.size() && e.bits[i + j] != 0 ? 1u : 0u);
                        char two[3];
                        std::snprintf(two, sizeof two, "%02x", v);
                        hex += two;
                    }
                    std::printf("{\"ev\":\"block\",\"decoder\":\"cxx\",\"burst\":%d,\"index\":%d,\"sample\":%lld,"
                                "\"bits\":%zu,\"bits_hex\":\"%s\"}\n",
                                burst, e.block_index, static_cast<long long>(e.sample), e.bits.size(), hex.c_str());
                    break;
                }
                case RxEventKind::BurstCompleted:
                    std::printf("{\"ev\":\"burst_completed\",\"decoder\":\"cxx\",\"burst\":%d,\"sample\":%lld,"
                                "\"start_sample\":%lld,\"wn\":%d,\"il\":\"%s\",\"k\":%d,\"blocks\":%d,\"reason\":\"%s\","
                                "\"payload_bits\":%lld,\"snr_db\":%.1f,\"cpu_ms\":%.1f}\n",
                                burst, static_cast<long long>(e.sample), static_cast<long long>(started_at),
                                e.waveform, il_name(e.interleaver), e.constraint_length, e.blocks,
                                reason_name(e.reason), static_cast<long long>(e.payload_bits), snr_db(started_at, e.sample), burst_cpu);
                    break;
                default:
                    break;
            }
        }
    }

    // Feeds samples in chunks of 96, charging the processor time to the burst when one was open as the
    // call began, and to the search otherwise.
    void feed(const float* s, size_t n) {
        constexpr size_t chunk = 96;
        for (size_t i = 0; i < n; i += chunk) {
            std::span<const float> part(s + i, std::min(chunk, n - i));
            size_t done = 0;
            while (done < part.size()) {
                bool open = rx.state() != RxState::Searching;
                if (!open) burst_cpu = 0;  // a burst's time starts on leaving the search, preamble reading included
                double t0 = cpu_ms();
                size_t used = 0;
                rx.process(part.subspan(done), used);
                double dt = cpu_ms() - t0;
                if (open) { burst_cpu += dt; cpu_in_bursts += dt; } else { cpu_search += dt; }
                done += used;
                drain();
            }
        }
    }
};

int usage() {
    std::fputs("usage: ms110d-decode [--write-resampled FILE.wav] [--max-interleaver ultra-short|short|medium|long] FILE.wav\n", stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, write_resampled;
    InterleaverKind max_il = InterleaverKind::Long;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--write-resampled" && i + 1 < argc) {
            write_resampled = argv[++i];
        } else if (a == "--max-interleaver" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "ultra-short") max_il = InterleaverKind::UltraShort;
            else if (v == "short") max_il = InterleaverKind::Short;
            else if (v == "medium") max_il = InterleaverKind::Medium;
            else if (v == "long") max_il = InterleaverKind::Long;
            else return usage();
        } else if (a == "--version") {
            std::printf("%s\n", version_text().c_str());
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else if (path.empty()) {
            path = a;
        } else {
            return usage();
        }
    }
    if (path.empty()) return usage();

    std::vector<double> raw;
    int rate = 0;
    std::string error;
    if (!ms110d_tool::read_wav(path, raw, rate, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::vector<std::int16_t> pcm;
    if (!ms110d_tool::resample_to_9600(raw, rate, pcm)) {
        std::fprintf(stderr, "%s: sample rate %d Hz is below 9600 Hz\n", path.c_str(), rate);
        return 1;
    }
    raw = {};
    if (!write_resampled.empty()) write_wav16(write_resampled, pcm);
    std::vector<float> audio(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) audio[i] = static_cast<float>(pcm[i]) / 32768.0f;
    pcm = {};

    RxOptions options;
    options.max_interleaver = max_il;
    Tool t;
    t.audio = &audio;
    t.floor_power = noise_floor_power(audio);
    size_t bytes = 0;
    if (Receiver::storage_bytes(options, bytes) != Status::Ok) { std::fputs("storage_bytes failed\n", stderr); return 1; }
    t.memory.resize(bytes);
    if (t.rx.configure(options, t.memory) != Status::Ok) { std::fputs("configure failed\n", stderr); return 1; }

    double c0 = cpu_ms();
    t.feed(audio.data(), audio.size());
    std::vector<float> silence(9600 * 3, 0.0f);
    t.feed(silence.data(), silence.size());
    if (t.rx.state() != RxState::Searching) {
        t.rx.terminate();
        std::vector<float> one(96, 0.0f);
        t.feed(one.data(), one.size());
    }
    double total = cpu_ms() - c0;
    std::printf("{\"ev\":\"summary\",\"decoder\":\"cxx\",\"version\":\"%s\",\"audio_s\":%.2f,\"bursts\":%d,"
                "\"cpu_ms_total\":%.1f,\"cpu_ms_in_bursts\":%.1f,\"cpu_ms_search\":%.1f}\n",
                version_text().c_str(), static_cast<double>(audio.size()) / 9600.0, t.bursts, total,
                t.cpu_in_bursts, t.cpu_search);
    return 0;
}
