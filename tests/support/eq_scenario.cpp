// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "eq_scenario.hpp"

#include "tables.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ms110d_test {

using namespace m0lte::ms110d;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- Complex32 arithmetic as the C#

Sample cmul(Sample a, Sample b) {
    return Sample{(a.real() * b.real()) - (a.imag() * b.imag()), (a.real() * b.imag()) + (a.imag() * b.real())};
}
Sample cadd(Sample a, Sample b) { return Sample{a.real() + b.real(), a.imag() + b.imag()}; }
Sample csub(Sample a, Sample b) { return Sample{a.real() - b.real(), a.imag() - b.imag()}; }
Sample cscale(Sample a, float s) { return Sample{a.real() * s, a.imag() * s}; }
Sample cconj(Sample a) { return Sample{a.real(), -a.imag()}; }
float mag2(Sample a) { return (a.real() * a.real()) + (a.imag() * a.imag()); }
float cabs(Sample a) { return std::sqrt((a.real() * a.real()) + (a.imag() * a.imag())); }

// ---------------------------------------------------------------- the generator

struct Rng {
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double gaussian() {
        double a = 0.0;
        for (int i = 0; i < 12; i++) a += uniform();
        return a - 6.0;
    }
    std::uint64_t s;
};

const std::vector<std::vector<double>>& channels() {
    static const std::vector<std::vector<double>> c = {
        {0.15, 0.05, 1.0, 0.0, 0.5, 0.3, -0.1, 0.25, 0.2, -0.1, 0.05, 0.05},
        {0.15, 0.05, 1.0, 0.0, 0.5, 0.3, -0.1, 0.25, 0.2, -0.1, 0.05, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.45, -0.2},
        {0.0, 0.0, 0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, 0.0, 0.0, 0.15, 0.0, 0.1, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, -0.3},
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, -0.3},
    };
    return c;
}

constexpr double step_re = 0.999998;
constexpr double step_im = 0.002;

struct Burst {
    std::vector<Sample> tx, rotor;
    std::vector<int> bit;
    int start = 0;
    bool ok = true;
};

Burst make_burst(const EqScenario& s, const fs::path& ref_dir) {
    Burst b;
    const int total = s.train_len + s.k + (s.frames * (s.u + s.k));
    b.tx.assign(static_cast<std::size_t>(total), Sample{});
    b.rotor.assign(static_cast<std::size_t>(s.frames * s.u), Sample{});
    b.bit.assign(static_cast<std::size_t>(s.frames * s.u), 0);
    b.start = s.train_len + s.k;
    const auto* spec = tables::find_probe_spec(s.k);
    if (spec == nullptr) {
        b.ok = false;
        return b;
    }
    auto probe = [&](int i, bool boundary) { return tables::probe_symbol(*spec, i, boundary); };
    if (s.source == "synthetic") {
        Rng rng(s.seed);
        for (int i = 0; i < s.train_len; i++) b.tx[static_cast<std::size_t>(i)] = tables::psk8[rng.next() & 7];
        for (int i = 0; i < s.k; i++) b.tx[static_cast<std::size_t>(s.train_len + i)] = probe(i, false);
        for (int f = 0; f < s.frames; f++) {
            const int bas = b.start + (f * (s.u + s.k));
            for (int u = 0; u < s.u; u++) {
                const std::uint64_t v = rng.next();
                const int idx = static_cast<int>(v & 7);
                const int bit = ((v >> 33) & 1) != 0 ? -1 : 1;
                const Sample rot = tables::psk8[static_cast<std::size_t>(idx)];
                b.rotor[static_cast<std::size_t>((f * s.u) + u)] = rot;
                b.bit[static_cast<std::size_t>((f * s.u) + u)] = bit;
                b.tx[static_cast<std::size_t>(bas + u)] = cscale(rot, static_cast<float>(bit));
            }
            for (int i = 0; i < s.k; i++) b.tx[static_cast<std::size_t>(bas + s.u + i)] = probe(i, f % 4 == 3);
        }
        return b;
    }
    auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const fs::path dir = ref_dir / s.source;
    const auto sym = read(dir / "09_symbols.sym");
    const auto probes = read(dir / "10_probes.cf32");
    const auto scr = read(dir / "07_scramble.sym");
    if (sym.empty() || probes.empty() || scr.empty()) {
        b.ok = false;
        return b;
    }
    constexpr int pre = 1728;
    std::size_t probe_at = 0;
    auto next_probe = [&]() {
        float v[2];
        std::memcpy(v, probes.data() + (probe_at * 8), 8);
        probe_at++;
        return Sample{v[0], v[1]};
    };
    std::size_t n = 0;
    for (int i = pre - s.train_len; i < pre; i++) b.tx[n++] = tables::psk8[sym[static_cast<std::size_t>(i)]];
    std::size_t p = pre;
    for (int i = 0; i < s.k; i++) {
        if (sym[p++] != 255) b.ok = false;
        b.tx[n++] = next_probe();
    }
    std::size_t du = 0;
    for (int f = 0; f < s.frames; f++) {
        for (int u = 0; u < s.u; u++) {
            const int idx = sym[p++];
            const int rot_idx = scr[du];
            const int diff = (idx - rot_idx + 8) & 7;
            if (diff != 0 && diff != 4) b.ok = false;
            b.rotor[static_cast<std::size_t>((f * s.u) + u)] = tables::psk8[static_cast<std::size_t>(rot_idx)];
            b.bit[static_cast<std::size_t>((f * s.u) + u)] = diff == 0 ? 1 : -1;
            b.tx[n++] = tables::psk8[static_cast<std::size_t>(idx)];
            du++;
        }
        for (int i = 0; i < s.k; i++) {
            if (sym[p++] != 255) b.ok = false;
            b.tx[n++] = next_probe();
        }
    }
    return b;
}

std::vector<Sample> make_samples(const EqScenario& s, const Burst& b) {
    const auto& c = channels()[static_cast<std::size_t>(s.chan)];
    const int taps = static_cast<int>(c.size() / 2);
    const int count = (2 * static_cast<int>(b.tx.size())) + 64;
    std::vector<Sample> x(static_cast<std::size_t>(count));
    Rng noise(s.seed + 1);
    const double sd = std::sqrt(s.noise_var / 2.0);
    double rr = 1.0, ri = 0.0;
    for (int m = 0; m < count; m++) {
        double ar = 0.0, ai = 0.0;
        for (int k = 0; k < taps; k++) {
            const int q = m - k;
            if (q < 0 || (q & 1) != 0 || (q >> 1) >= static_cast<int>(b.tx.size())) continue;
            const Sample t = b.tx[static_cast<std::size_t>(q >> 1)];
            const double sr = static_cast<double>(t.real()), si = static_cast<double>(t.imag());
            const double cr = c[static_cast<std::size_t>(2 * k)], ci = c[static_cast<std::size_t>((2 * k) + 1)];
            ar += (cr * sr) - (ci * si);
            ai += (cr * si) + (ci * sr);
        }
        double yr = (ar * rr) - (ai * ri);
        double yi = (ar * ri) + (ai * rr);
        if (s.noise_var > 0) {
            yr += noise.gaussian() * sd;
            yi += noise.gaussian() * sd;
        }
        x[static_cast<std::size_t>(m)] = Sample{static_cast<float>(yr), static_cast<float>(yi)};
        const double nr = (rr * step_re) - (ri * step_im);
        const double ni = (rr * step_im) + (ri * step_re);
        rr = nr;
        ri = ni;
    }
    return x;
}

std::uint64_t fnv(const std::vector<Sample>& v) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const Sample& c : v) {
        const float parts[2] = {c.real(), c.imag()};
        for (float f : parts) {
            std::uint32_t w;
            std::memcpy(&w, &f, 4);
            for (int i = 0; i < 4; i++) {
                h ^= (w >> (8 * i)) & 0xFF;
                h *= 1099511628211ULL;
            }
        }
    }
    return h;
}

void make_window(const std::vector<Sample>& x, int symbol, int lead, std::span<Sample> window) {
    for (std::size_t i = 0; i < window.size(); i++) {
        const int m = (2 * symbol) + lead - static_cast<int>(i);
        window[i] = m >= 0 && m < static_cast<int>(x.size()) ? x[static_cast<std::size_t>(m)] : Sample{};
    }
}

void push(std::vector<Sample>& decisions, Sample d) {
    for (std::size_t j = decisions.size(); j-- > 1;) decisions[j] = decisions[j - 1];
    if (!decisions.empty()) decisions[0] = d;
}

Sample slice(Sample v) { return Sample{v.real() >= 0 ? 1.0f : -1.0f, 0.0f}; }

constexpr float dd_gate = 0.5f;

}  // namespace

std::vector<std::uint8_t> eq_storage(int ff, int fb) { return std::vector<std::uint8_t>(Dfe::storage_bytes(ff, fb)); }

EqResult run_eq_scenario(const EqScenario& s, const fs::path& ref_dir) {
    EqResult r;
    const Burst b = make_burst(s, ref_dir);
    if (!b.ok) return r;
    const std::vector<Sample> x = make_samples(s, b);
    r.txhash = fnv(b.tx);
    r.xhash = fnv(x);

    auto storage = eq_storage(s.ff, s.fb);
    Dfe dfe;
    if (!dfe.configure(s.ff, s.fb, storage)) return r;
    const int ff = s.ff, fb = s.fb, k = s.k, u = s.u;
    std::vector<Sample> window(static_cast<std::size_t>(ff)), past(static_cast<std::size_t>(fb));

    const int known_len = s.train_len + k;
    dfe.begin_training();
    for (int n = 64; n < known_len; n++) {
        make_window(x, n, s.lead, window);
        for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(n - 1 - j)];
        dfe.add_training_row(window, past, b.tx[static_cast<std::size_t>(n)]);
    }
    const bool rls = s.pass != "nlms";
    if (rls) {
        dfe.begin_rls(s.lambda, 1.0f);
        dfe.seed_rls_from_training(s.init_ridge, 1.0f, 0.0f);
    }
    r.init_solved = dfe.solve_training(s.init_ridge, false, 0.0f);
    dfe.begin_training();
    std::vector<Sample> decisions(static_cast<std::size_t>(fb));
    for (int j = 0; j < fb; j++) decisions[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(known_len - 1 - j)];

    double mse0 = 0;
    for (int i = 0; i < k; i++) {
        const int n = s.train_len + i;
        make_window(x, n, s.lead, window);
        for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(n - 1 - j)];
        const Sample y = dfe.equalise(window, past);
        const Sample err = csub(y, b.tx[static_cast<std::size_t>(n)]);
        mse0 += static_cast<double>(mag2(err));
    }
    r.init_mse = mse0 / k;
    r.init_ffe = dfe.ff_energy();

    const float rls_weight = s.pass == "fading" ? 1.0f : 0.1f;
    const int n_taps = ff + fb;
    std::vector<Sample> taps_prev(static_cast<std::size_t>(n_taps)), taps_cur(static_cast<std::size_t>(n_taps));
    std::vector<Sample> start_taps(static_cast<std::size_t>(n_taps)), end_taps(static_cast<std::size_t>(n_taps));
    std::vector<Sample> probe(static_cast<std::size_t>(k)), y_out(static_cast<std::size_t>(u));
    std::vector<Sample> pass1(static_cast<std::size_t>(u)), pass2(static_cast<std::size_t>(u));
    std::vector<Sample> frame_start(static_cast<std::size_t>(fb)), mid(static_cast<std::size_t>(n_taps));

    for (int f = 0; f < s.frames; f++) {
        const int bas = b.start + (f * (u + k));
        const int probe_at = bas + u;
        for (int i = 0; i < k; i++) probe[static_cast<std::size_t>(i)] = b.tx[static_cast<std::size_t>(probe_at + i)];

        dfe.snapshot_taps(start_taps);
        struct ProbeCtx {
            const std::vector<Sample>* x;
            int lead, at;
        } pctx{&x, s.lead, probe_at};
        const WindowFill probe_fill = +[](void* c, int i, std::span<Sample> w) noexcept {
            const auto& pc = *static_cast<ProbeCtx*>(c);
            make_window(*pc.x, pc.at + i, pc.lead, w);
        };
        const ProbeStats ps = dfe.train_on_known(probe, fb, probe_fill, &pctx, 6.0f);
        const Sample probe_phase = ps.correlation;
        const double mse = static_cast<double>(ps.error_energy);
        const int rows = ps.rows;

        if (s.extras && f == 1) {
            dfe.snapshot_training();
            std::vector<Sample> keep(static_cast<std::size_t>(n_taps));
            dfe.snapshot_taps(keep);
            dfe.begin_training();
            for (int i = fb; i < k; i++) {
                make_window(x, probe_at + i, s.lead, window);
                for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = probe[static_cast<std::size_t>(i - 1 - j)];
                dfe.add_training_row(window, past, probe[static_cast<std::size_t>(i)], 1.0f);
            }
            r.nested_seen = true;
            r.nested_solved = dfe.solve_training(0.01f, true, 0.0f);
            r.nested_ffe = dfe.ff_energy();
            dfe.load_taps(keep);
            dfe.restore_training();
        }

        if (rls) dfe.seed_rls_from_training(s.track_ridge, 1.0f, 0.0f);
        const bool solved = dfe.solve_training(s.track_ridge, true, 0.0f);

        Sample post_phase{};
        for (int i = fb; i < k; i++) {
            make_window(x, probe_at + i, s.lead, window);
            for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = probe[static_cast<std::size_t>(i - 1 - j)];
            post_phase = cadd(post_phase, cmul(dfe.equalise(window, past), cconj(probe[static_cast<std::size_t>(i)])));
        }
        const float pp_abs = cabs(post_phase);
        if (pp_abs / static_cast<float>(rows) >= 0.10f) {
            dfe.rotate_taps(Sample{post_phase.real() / pp_abs, -post_phase.imag() / pp_abs});
        }
        dfe.snapshot_taps(end_taps);
        EqProbe pr;
        pr.solved = solved;
        pr.corr = probe_phase;
        pr.mse = mse / rows;
        pr.error_energy = mse;
        pr.rows = rows;
        pr.post = post_phase;
        pr.ffe = dfe.ff_energy();
        r.probes.push_back(pr);

        frame_start = decisions;
        dfe.begin_training();
        const float rw = rls_weight;
        auto data_symbol = [&](int uu, Sample& rotor, Sample& descr, Sample& clean) {
            rotor = b.rotor[static_cast<std::size_t>((f * u) + uu)];
            make_window(x, bas + uu, s.lead, window);
            const Sample y = dfe.equalise(window, decisions);
            descr = cmul(y, cconj(rotor));
            clean = slice(descr);
        };
        Sample rotor, descr, clean;
        if (s.pass == "fading") {
            for (int uu = 0; uu < u; uu++) {
                const float a = (static_cast<float>(uu) + (static_cast<float>(k) / 2.0f)) / static_cast<float>(u + k);
                const float inv = 1 - a;
                for (int i = 0; i < n_taps; i++) {
                    const auto si = static_cast<std::size_t>(i);
                    taps_cur[si] = cadd(cscale(start_taps[si], inv), cscale(end_taps[si], a));
                }
                if (uu == 0) {
                    dfe.load_taps(taps_cur);
                } else {
                    dfe.translate_taps(taps_prev, taps_cur);
                }
                taps_prev = taps_cur;
                data_symbol(uu, rotor, descr, clean);
                y_out[static_cast<std::size_t>(uu)] = descr;
                if (mag2(csub(descr, clean)) < dd_gate) {
                    dfe.rls_update(window, decisions, cmul(clean, rotor), rw);
                    dfe.add_training_row(window, decisions, cmul(clean, rotor), 0.25f);
                }
                push(decisions, cmul(clean, rotor));
            }
        } else if (s.pass == "nlms") {
            for (int uu = 0; uu < u; uu++) {
                data_symbol(uu, rotor, descr, clean);
                y_out[static_cast<std::size_t>(uu)] = descr;
                dfe.nlms(window, decisions, cmul(clean, rotor), s.mu);
                if (mag2(csub(descr, clean)) < dd_gate) {
                    dfe.add_training_row(window, decisions, cmul(clean, rotor), 0.25f);
                }
                push(decisions, cmul(clean, rotor));
            }
        } else {
            for (int uu = 0; uu < u; uu++) {
                data_symbol(uu, rotor, descr, clean);
                pass1[static_cast<std::size_t>(uu)] = descr;
                dfe.rls_update(window, decisions, cmul(clean, rotor), rw);
                if (mag2(csub(descr, clean)) < dd_gate) {
                    dfe.add_training_row(window, decisions, cmul(clean, rotor), 0.25f);
                }
                push(decisions, cmul(clean, rotor));
            }
            dfe.load_taps(start_taps);
            decisions = frame_start;
            for (int uu = 0; uu < u; uu++) {
                data_symbol(uu, rotor, descr, clean);
                pass2[static_cast<std::size_t>(uu)] = descr;
                dfe.rls_update(window, decisions, cmul(clean, rotor), rw);
                push(decisions, cmul(clean, rotor));
            }
            for (int i = 0; i < n_taps; i++) {
                const auto si = static_cast<std::size_t>(i);
                mid[si] = cscale(cadd(start_taps[si], end_taps[si]), 0.5f);
            }
            dfe.load_taps(mid);
            decisions = frame_start;
            for (int uu = 0; uu < u; uu++) {
                data_symbol(uu, rotor, descr, clean);
                const auto su = static_cast<std::size_t>(uu);
                y_out[su] = cscale(cadd(cadd(pass1[su], pass2[su]), descr), 1.0f / 3.0f);
                dfe.rls_update(window, decisions, cmul(clean, rotor), rw);
                push(decisions, cmul(clean, rotor));
            }
        }

        if (s.extras && f == 2) {
            const std::vector<float> pv(static_cast<std::size_t>(fb), 0.05f);
            for (int uu = 8; uu < 16 && uu < u; uu++) {
                make_window(x, bas + uu, s.lead, window);
                for (int j = 0; j < fb; j++) {
                    past[static_cast<std::size_t>(j)] = cscale(b.tx[static_cast<std::size_t>(bas + uu - 1 - j)], 0.9f);
                }
                dfe.add_training_row(window, past, pv, b.tx[static_cast<std::size_t>(bas + uu)], 0.5f);
            }
        }

        dfe.symmetrize_p(10.0f);
        dfe.load_taps(end_taps);
        for (int i = 0; i < k; i++) push(decisions, probe[static_cast<std::size_t>(i)]);

        std::string dline;
        for (int uu = 0; uu < u; uu++) {
            const Sample yy = y_out[static_cast<std::size_t>(uu)];
            if (!s.quiet) dline.push_back(yy.real() >= 0 ? '+' : '-');
            if (f >= 2) {
                r.counted++;
                if ((yy.real() >= 0 ? 1 : -1) != b.bit[static_cast<std::size_t>((f * u) + uu)]) r.errors++;
            }
        }
        if (!s.quiet) {
            r.y.push_back(y_out);
            r.d.push_back(dline);
        }
    }
    r.taps.assign(static_cast<std::size_t>(n_taps), Sample{});
    dfe.snapshot_taps(r.taps);
    return r;
}

TirResult run_tir_scenario(const TirScenario& t) {
    EqScenario s;
    s.name = t.name;
    s.k = 32;
    s.u = 64;
    s.frames = 6;
    s.ff = 24;
    s.fb = 12;
    s.lead = 13;
    s.track_ridge = 0.15f;
    s.noise_var = t.noise_var;
    s.seed = 77;
    s.chan = t.chan;
    s.train_len = 192;
    TirResult res;
    const Burst b = make_burst(s, {});
    const std::vector<Sample> x = make_samples(s, b);
    res.xhash = fnv(x);
    auto storage = eq_storage(s.ff, s.fb);
    Dfe dfe;
    dfe.configure(s.ff, s.fb, storage);
    std::vector<Sample> window(static_cast<std::size_t>(s.ff)), hist(static_cast<std::size_t>(s.fb));
    for (int pass = 0; pass < (t.second_only_lag > 0 ? 2 : 1); pass++) {
        dfe.begin_training();
        for (int f = 0; f < 4; f++) {
            const int frame_at = b.start + (f * (s.u + s.k));
            for (int i = s.fb; i < s.u + s.k; i++) {
                make_window(x, frame_at + i, s.lead, window);
                for (int j = 0; j < s.fb; j++) hist[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(frame_at + i - 1 - j)];
                dfe.add_training_row(window, hist, b.tx[static_cast<std::size_t>(frame_at + i)], 1.0f);
            }
        }
        const int only = pass == 1 ? t.second_only_lag : 0;
        TirResult::Pass p;
        p.t = dfe.solve_training_tir(1e-3f, 0.0f, std::min(s.fb, s.u / 2), t.allow_pair, only);
        p.ffe = dfe.ff_energy();
        res.passes.push_back(p);
    }
    const std::vector<Sample> zero(static_cast<std::size_t>(s.fb));
    const int bas = b.start + (4 * (s.u + s.k));
    for (int uu = 0; uu < s.u; uu++) {
        make_window(x, bas + uu, s.lead, window);
        res.y.push_back(dfe.equalise(window, zero));
    }
    return res;
}

namespace {

void misc_rows(Dfe& dfe, const Burst& b, const std::vector<Sample>& x, int lead, int ff, int fb, int from, int to, float w) {
    std::vector<Sample> window(static_cast<std::size_t>(ff)), past(static_cast<std::size_t>(fb));
    for (int n = from; n < to; n++) {
        make_window(x, n, lead, window);
        for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(n - 1 - j)];
        dfe.add_training_row(window, past, b.tx[static_cast<std::size_t>(n)], w);
    }
}

void misc_updates(Dfe& dfe, const Burst& b, const std::vector<Sample>& x, int lead, int ff, int fb, int from, int to) {
    std::vector<Sample> window(static_cast<std::size_t>(ff)), past(static_cast<std::size_t>(fb));
    for (int n = from; n < to; n++) {
        make_window(x, n, lead, window);
        for (int j = 0; j < fb; j++) past[static_cast<std::size_t>(j)] = b.tx[static_cast<std::size_t>(n - 1 - j)];
        dfe.rls_update(window, past, b.tx[static_cast<std::size_t>(n)], 1.0f);
    }
}

void push_taps(std::vector<MiscLine>& out, const char* label, const Dfe& dfe) {
    std::vector<Sample> taps(static_cast<std::size_t>(dfe.ff_taps() + dfe.fb_taps()));
    dfe.snapshot_taps(taps);
    MiscLine l{label, {}};
    for (const Sample& t : taps) {
        l.values.push_back(t.real());
        l.values.push_back(t.imag());
    }
    out.push_back(std::move(l));
}

}  // namespace

std::vector<MiscLine> run_misc_scenario(std::uint64_t& xhash) {
    EqScenario s;
    s.name = "misc";
    s.k = 32;
    s.u = 64;
    s.frames = 4;
    s.ff = 12;
    s.fb = 4;
    s.lead = 7;
    s.noise_var = 0.05;
    s.seed = 91;
    s.chan = 0;
    s.train_len = 192;
    const Burst b = make_burst(s, {});
    const std::vector<Sample> x = make_samples(s, b);
    xhash = fnv(x);
    const int ff = s.ff, fb = s.fb, lead = s.lead;
    std::vector<MiscLine> out;

    auto st = eq_storage(ff, fb);
    Dfe dfe;
    dfe.configure(ff, fb, st);
    dfe.begin_training();
    misc_rows(dfe, b, x, lead, ff, fb, 64, 224, 1.0f);
    dfe.begin_rls(0.98f, 50.0f);
    dfe.seed_rls_from_training(1e-3f, 1.0f, 0.05f);
    const bool ok = dfe.solve_training(1e-3f, false, 0.05f);
    out.push_back({"noisesolve-ok", {ok ? 1.0f : 0.0f, 0.0f}});
    std::vector<Sample> a(static_cast<std::size_t>(ff + fb)), c2(static_cast<std::size_t>(ff + fb));
    dfe.snapshot_taps(a);
    push_taps(out, "noisesolve", dfe);
    misc_updates(dfe, b, x, lead, ff, fb, 230, 270);
    dfe.snapshot_taps(c2);
    push_taps(out, "rls", dfe);
    dfe.load_interpolated_taps(a, c2, 0.3f);
    push_taps(out, "interp", dfe);

    for (int variant = 0; variant < 2; variant++) {
        auto sd = eq_storage(ff, fb);
        Dfe d;
        d.configure(ff, fb, sd);
        d.begin_training();
        misc_rows(d, b, x, lead, ff, fb, 100, 116, 1.0f);
        d.begin_rls(0.98f, 1.0f);
        d.seed_rls_from_training(1e-6f, 1.0f, 0.0f);
        misc_updates(d, b, x, lead, ff, fb, 130, 140);
        if (variant == 1) d.symmetrize_p(10.0f);
        misc_updates(d, b, x, lead, ff, fb, 140, 150);
        push_taps(out, variant == 0 ? "p-uncapped" : "p-capped", d);
    }

    auto stt = eq_storage(ff, fb);
    Dfe t;
    t.configure(ff, fb, stt);
    t.begin_training();
    misc_rows(t, b, x, lead, ff, fb, 64, 128, 1.0f);
    t.snapshot_training();
    t.begin_training();
    misc_rows(t, b, x, lead, ff, fb, 128, 140, 3.0f);
    t.restore_training();
    misc_rows(t, b, x, lead, ff, fb, 200, 230, 1.0f);
    const TirSolve r = t.solve_training_tir(1e-3f, 0.05f, 4, true, 0);
    out.push_back({"tir-after-restore",
                   {r.solved ? 1.0f : 0.0f, static_cast<float>(r.lag), r.coefficient.real(), r.coefficient.imag(),
                    static_cast<float>(r.lag2), 0.0f, r.coefficient2.real(), r.coefficient2.imag(), r.sse_null, r.sse_tir,
                    t.ff_energy(), 0.0f}});
    return out;
}

}  // namespace ms110d_test
