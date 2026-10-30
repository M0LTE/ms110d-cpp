#!/usr/bin/env python3
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Curates the layer 4 real-audio corpus. See docs/dev/corpus.md.

  scripts/corpus/curate.py [--slots-dir DIR | --pull] [--out DIR] [options]

Input is a directory of slot directories (<UTC>/audio.wav at 12 kHz mono 16-bit, plus meta.json).
--pull first copies them from the recorder named by the environment (CORPUS_REMOTE_CMD, a command
prefix that runs a command there, and CORPUS_REMOTE_SLOTS, the slots directory there).
Each slot is resampled to 9600 Hz, the C# receiver finds the bursts, each is cut out with a margin
and written as 16-bit mono WAV, and the C# receiver decodes the cut file for the expected results.
Needs python3 with numpy, and a .NET 10 SDK. Deterministic: the same slots give the same files.
"""
import argparse, hashlib, json, os, shlex, subprocess, sys, wave
from datetime import datetime, timedelta, timezone

import numpy as np

RATE_IN, RATE_OUT = 12000, 9600
UP, DOWN = 4, 5            # 9600 / 12000 = 4 / 5
TAPS = 801                 # odd, so the group delay is a whole number of samples
KAISER_BETA = 8.0
CUTOFF_HZ = 4400.0         # at the 48 kHz intermediate rate; the output Nyquist is 4800
MARGINS_BEFORE_S = (6.0, 4.0, 3.0, 2.0)   # tried in turn; the receiver reports a burst some seconds after it began
MARGIN_AFTER_S = 1.5
FORMAT_VERSION = 1
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def design_filter():
    """Kaiser windowed sinc, DC gain UP. Pure numpy float64, so it is the same everywhere."""
    n = np.arange(TAPS) - (TAPS - 1) / 2
    fc = CUTOFF_HZ / (RATE_IN * UP)
    h = 2 * fc * np.sinc(2 * fc * n) * np.kaiser(TAPS, KAISER_BETA)
    return h * (UP / h.sum())


def resample(x):
    """12 kHz -> 9600 Hz: zero-stuff by 4, FIR low-pass, keep every 5th, evaluated as a polyphase sum.
    y[m] = sum_n h[n] u[5m + D - n] with u[4i] = x[i] and D the filter's group delay, so the output is
    time aligned with the input (output sample m is at input time m * 1.25 samples)."""
    h = design_filter()
    d = (TAPS - 1) // 2
    nout = len(x) * UP // DOWN
    pad = TAPS // UP + 2
    xp = np.concatenate([np.zeros(pad), x.astype(np.float64), np.zeros(pad)])
    m = np.arange(nout)
    t = DOWN * m + d
    r = t % UP
    base = t // UP + pad
    y = np.zeros(nout)
    for phase in range(UP):
        hp = h[phase::UP]
        sel = np.nonzero(r == phase)[0]
        if len(sel) == 0:
            continue
        # taps j = 0..len(hp)-1 read x at base - j
        idx = base[sel][:, None] - np.arange(len(hp))[None, :]
        y[sel] = np.einsum("ij,j->i", xp[idx], hp)
    return y


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != RATE_IN:
            sys.exit(f"{path}: need 12 kHz mono 16-bit")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0


def write_wav(path, samples_i16):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE_OUT)
        w.writeframes(samples_i16.astype("<i2").tobytes())


def quantise(x):
    return np.clip(np.rint(x * 32768.0), -32768, 32767).astype(np.int16)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run_tool(dll, mode, wav):
    out = subprocess.run(["dotnet", dll, "corpus", mode, wav], check=True, capture_output=True, text=True).stdout
    res = []
    for line in out.splitlines():
        if line.startswith("burst "):
            res.append(dict(kv.split("=", 1) for kv in line.split()[1:]))
    return res


INT_KEYS = ("wn", "k", "blocks", "bytes", "good_frames", "corrected", "crc_failures", "rs_failures")


def stream_dropouts(log_path):
    """(from, to) UTC spans in which the recorder's log says the audio stream dropped or fell behind.
    A burst that overlaps one may have lost audio, so it is flagged `dropout` in the manifest. The log
    reports a drop some seconds after it happens (10 s assumed), and a falling-behind report covers the
    minute before it."""
    spans = []
    if os.path.exists(log_path):
        for line in open(log_path, errors="replace"):
            behind = "BEHIND real time" in line
            if behind or ("stream from" in line and " ended" in line):
                try:
                    t = datetime.fromisoformat(line.split()[0])
                except ValueError:
                    continue
                spans.append((t - timedelta(seconds=70 if behind else 10), t))
    return spans


def noise_floor_power(x):
    """Median power of the quietest fifth of 0.5 s windows across the slot."""
    n = RATE_OUT // 2
    k = len(x) // n
    p = np.sort((x[: k * n].reshape(k, n) ** 2).mean(axis=1))
    return float(np.median(p[: max(1, k // 5)])) + 1e-18


def pull(args):
    cmd = os.environ.get("CORPUS_REMOTE_CMD")
    rdir = os.environ.get("CORPUS_REMOTE_SLOTS")
    if not cmd or not rdir:
        sys.exit("--pull needs CORPUS_REMOTE_CMD (command prefix) and CORPUS_REMOTE_SLOTS (slots directory) in the environment")
    prefix = shlex.split(cmd)

    def remote(c, binary=False):
        return subprocess.run(prefix + ["sh", "-c", c], check=True, capture_output=True).stdout

    names = remote(f"ls {shlex.quote(rdir)}").decode().split()
    os.makedirs(args.slots_dir, exist_ok=True)
    for n in names:
        if args.only and n not in args.only:
            continue
        d = os.path.join(args.slots_dir, n)
        if os.path.exists(os.path.join(d, "meta.json")):
            continue
        os.makedirs(d, exist_ok=True)
        for f in ("audio.wav", "decode.log", "meta.json"):
            with open(os.path.join(d, f), "wb") as fh:
                fh.write(remote(f"cat {shlex.quote(rdir + '/' + n + '/' + f)}"))
        print("pulled", n)


def write_committed(args, manifest):
    """The small set kept in the repo for quick tests: the bursts named by --committed-ids."""
    import shutil
    by_id = {e["id"]: e for e in manifest["bursts"]}
    chosen = []
    for i in args.committed_ids:
        if i not in by_id:
            sys.exit(f"--committed-ids: {i} is not in the corpus")
        chosen.append(by_id[i])
    used = sum(os.path.getsize(os.path.join(args.out, e["file"])) for e in chosen)
    if used > args.committed_max_bytes:
        sys.exit(f"committed set is {used} bytes, over the {args.committed_max_bytes} budget")
    shutil.rmtree(args.committed, ignore_errors=True)
    os.makedirs(os.path.join(args.committed, "bursts"))
    for e in chosen:
        shutil.copy(os.path.join(args.out, e["file"]), os.path.join(args.committed, e["file"]))
    sub = dict(manifest, bursts=chosen)
    with open(os.path.join(args.committed, "manifest.json"), "w") as f:
        json.dump(sub, f, indent=1, sort_keys=True); f.write("\n")
    print(f"committed set: {len(chosen)} bursts, {used/1e6:.2f} MB, in {args.committed}")


def main():
    cache = os.environ.get("MS110D_CORPUS_CACHE", os.path.expanduser("~/.cache/ms110d-corpus"))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slots-dir", default=os.path.join(cache, "slots"))
    ap.add_argument("--pull", action="store_true", help="copy slots from the recorder first")
    ap.add_argument("--only", nargs="*", help="slot directory names to use (default: all)")
    ap.add_argument("--out", default=os.path.join(cache, "curated"))
    ap.add_argument("--work", default=os.path.join(cache, "work"))
    ap.add_argument("--min-blocks", type=int, default=1, help="drop false alarms that decode fewer blocks")
    ap.add_argument("--committed", help="also write a small set for the repo (tests/data/corpus) to this directory")
    ap.add_argument("--committed-ids", nargs="*", default=["20261028T095800Z-10", "20261027T095800Z-03"],
                    help="bursts for the committed set (default: a strong WN4 error-free one and a WN3 one-frame one)")
    ap.add_argument("--committed-max-bytes", type=int, default=850_000)
    ap.add_argument("--max-per-wn", type=int, default=0, help="keep at most this many bursts per waveform (0 = all)")
    args = ap.parse_args()

    if args.pull:
        pull(args)
    os.makedirs(args.work, exist_ok=True)
    tool = os.path.join(args.work, "tool")
    subprocess.run(["dotnet", "build", "-c", "Release", "-v", "q", "-o", tool,
                    os.path.join(ROOT, "tools", "csharp-vectors")], check=True, stdout=subprocess.DEVNULL)
    dll = os.path.join(tool, "CsharpVectors.dll")

    slots = sorted(d for d in os.listdir(args.slots_dir)
                   if os.path.exists(os.path.join(args.slots_dir, d, "audio.wav")) and (not args.only or d in args.only))
    if not slots:
        sys.exit(f"no slots in {args.slots_dir}")
    os.makedirs(os.path.join(args.out, "bursts"), exist_ok=True)

    entries, per_wn = [], {}
    for slot in slots:
        sdir = os.path.join(args.slots_dir, slot)
        meta = json.load(open(os.path.join(sdir, "meta.json")))
        t0 = datetime.fromisoformat(meta["audioFrom"])
        print(f"{slot}: resampling")
        y = resample(read_wav(os.path.join(sdir, "audio.wav")))
        full = os.path.join(args.work, slot + ".wav")
        write_wav(full, quantise(y))
        floor = noise_floor_power(y)
        dropouts = stream_dropouts(os.path.join(sdir, "decode.log"))
        found = run_tool(dll, "find", full)
        print(f"{slot}: {len(found)} candidate bursts")
        for i, b in enumerate(found):
            if int(b["blocks"]) < args.min_blocks:
                continue
            wn = int(b["wn"])
            if args.max_per_wn and per_wn.get(wn, 0) >= args.max_per_wn:
                continue
            start, end = int(b["start"]), int(b["end"])
            bid = f"{slot}-{i:02d}"
            rel = f"bursts/{bid}.wav"
            path = os.path.join(args.out, rel)
            prev_end = int(found[i - 1]["end"]) if i else 0   # the receiver cannot lock again before this
            for margin in MARGINS_BEFORE_S:
                a = max(0, prev_end, start - int(margin * RATE_OUT))
                z = min(len(y), end + int(MARGIN_AFTER_S * RATE_OUT))
                seg = y[a:z]
                write_wav(path, quantise(seg))
                expected = run_tool(dll, "decode", path)
                if len(expected) == 1 and expected[0]["reason"] == "Eom":
                    break
            else:
                print(f"  {bid}: WN{wn} {b['reason']}: not a complete burst in the cut; skipped")
                os.remove(path)
                continue
            ex = expected[0]
            error_free = int(ex["good_frames"]) > 0 and int(ex["crc_failures"]) == 0 and int(ex["rs_failures"]) == 0
            t_a = t0 + timedelta(seconds=a / RATE_OUT)
            t_z = t0 + timedelta(seconds=z / RATE_OUT)
            dropout = any(lo <= t_z and t_a <= hi for lo, hi in dropouts)
            burst_s = (end - start) / RATE_OUT
            sig = float((y[start:end] ** 2).mean()) if end > start else 0.0
            snr = 10 * np.log10(max(sig - floor, 1e-18) / floor) if sig > floor else -20.0
            per_wn[wn] = per_wn.get(wn, 0) + 1
            entries.append({
                "id": bid, "file": rel, "slot": slot,
                "utc": (t0 + timedelta(seconds=a / RATE_OUT)).astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "waveform": wn, "interleaver": b["il"], "constraint_length": int(b["k"]),
                "snr_db_estimate": round(float(snr), 1),
                "duration_s": round((len(seg)) / RATE_OUT, 2),
                "samples": len(seg), "sha256": sha256_file(path),
                "expected": [{k: (int(v) if k in INT_KEYS else v) for k, v in e.items()} for e in expected],
                "error_free": error_free, "good_frames": int(expected[0]["good_frames"]),
                "dropout": dropout,
            })
            print(f"  {bid}: WN{wn} {b['il']} K{b['k']} {snr:+.1f} dB, {len(seg)/RATE_OUT:.1f} s, "
                  f"good frames {ex['good_frames']}, error_free={error_free}, dropout={dropout}")
    pin = {}
    for line in open(os.path.join(ROOT, "reference", "pin.txt")):
        if "=" in line and not line.startswith("#"):
            k, v = line.strip().split("=", 1); pin[k] = v
    manifest = {
        "format_version": FORMAT_VERSION,
        "sample_rate": RATE_OUT,
        "sample_format": "pcm_s16le mono WAV",
        "resampler": {"from": RATE_IN, "to": RATE_OUT, "method": f"polyphase {UP}/{DOWN}, Kaiser FIR {TAPS} taps beta {KAISER_BETA} cutoff {CUTOFF_HZ} Hz at {RATE_IN*UP} Hz, float64, rounded to int16"},
        "margins_s": {"before_candidates": list(MARGINS_BEFORE_S), "after": MARGIN_AFTER_S},
        "reference": {"nuget_version": pin.get("nuget_version"), "git_sha": pin.get("git_sha")},
        "payload_hash": "SHA-256 of the burst payload bits packed MSB first, zero padded",
        "bursts": entries,
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True); f.write("\n")
    if args.committed:
        write_committed(args, manifest)
    tot = sum(os.path.getsize(os.path.join(args.out, e["file"])) for e in entries)
    print(f"{len(entries)} bursts, {tot/1e6:.2f} MB, manifest in {args.out}/manifest.json")


if __name__ == "__main__":
    main()
