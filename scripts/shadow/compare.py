#!/usr/bin/env python3
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compares the C# and C++ decoders on shadow receiver slots. See docs/dev/shadow.md.

  compare.py SLOT_DIR                      read SLOT_DIR/cxx.jsonl and cs.jsonl, print the slot report (markdown)
  compare.py --cxx F --cs F                the same for two named files
  compare.py --daily SLOTS_DIR DATE        the day's summary over every slot directory named DATE* (YYYY-MM-DD or YYYYMMDD)
  -o FILE                                  write there instead of standard output

The inputs are the event lines of tools/ms110d-decode and `CsharpVectors shadow decode` (burst_started,
block, burst_completed, summary). Bursts are paired by overlap in time (they are the same audio). Per burst:

  both          at least one good IL2P frame from each decoder
  C# only       good frames from the C# and none from the C++
  C++ only      the reverse
  neither       no good frame from either (weak signal, or a lock on noise)
  C# lock only / C++ lock only   one decoder locked on something the other did not

The layer 4 rule (docs/dev/corpus.md) is applied to every pair with the C# as the reference: waveform,
interleaver and constraint length equal; end reason and block count equal when the C# has a good frame;
at most one good frame short per burst and 2 percent over the set; the payload equal on an error-free
burst. Frames the deframer drops (bad CRC) are never counted, so "every frame decoded passes its CRC" holds
by construction in this report.
"""
import argparse, json, os, sys
from datetime import datetime, timedelta, timezone

RATE = 9600


def read_events(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def bursts_of(events):
    """Completed (and refused) bursts of one decoder: the burst_completed events, plus a note of the summary."""
    done = [e for e in events if e["ev"] == "burst_completed"]
    summary = next((e for e in events if e["ev"] == "summary"), {})
    return done, summary


def overlap(a, b):
    lo = max(a["start_sample"], b["start_sample"])
    hi = min(a["sample"], b["sample"])
    return hi - lo


def pair_up(cs, cxx):
    """Greedy pairing by overlap, largest first. Returns (pairs, cs_alone, cxx_alone)."""
    cands = []
    for i, a in enumerate(cs):
        for j, b in enumerate(cxx):
            ov = overlap(a, b)
            if ov > 0:
                cands.append((ov, i, j))
    cands.sort(reverse=True)
    used_i, used_j, pairs = set(), set(), []
    for ov, i, j in cands:
        if i in used_i or j in used_j:
            continue
        used_i.add(i)
        used_j.add(j)
        pairs.append((cs[i], cxx[j]))
    pairs.sort(key=lambda p: p[0]["start_sample"])
    return (pairs,
            [a for i, a in enumerate(cs) if i not in used_i],
            [b for j, b in enumerate(cxx) if j not in used_j])


def rule_problems(a, b):
    """Layer 4 rule for one pair, a = C# reference, b = C++. Returns (problems, frames_short)."""
    p = []
    if b["wn"] != a["wn"]:
        p.append("waveform")
    if b["il"] != a["il"]:
        p.append("interleaver")
    if b["k"] != a["k"]:
        p.append("constraint length")
    if a["good_frames"] > 0:
        if b["reason"] != a["reason"]:
            p.append("end reason")
        if b["blocks"] != a["blocks"]:
            p.append("block count")
    short = max(0, a["good_frames"] - b["good_frames"])
    if short > 1:
        p.append(f"{short} frames short")
    if a.get("error_free") and b["sha256"] != a["sha256"]:
        p.append("payload differs on an error-free burst")
    return p, short


def classify(a, b):
    ga, gb = a["good_frames"] > 0, b["good_frames"] > 0
    return "both" if ga and gb else "C# only" if ga else "C++ only" if gb else "neither"


def analyse(cs_events, cxx_events):
    cs, cs_sum = bursts_of(cs_events)
    cxx, cxx_sum = bursts_of(cxx_events)
    pairs, cs_alone, cxx_alone = pair_up(cs, cxx)
    rows = []
    for a, b in pairs:
        probs, short = rule_problems(a, b)
        rows.append({"start": a["start_sample"], "cls": classify(a, b), "cs": a, "cxx": b,
                     "problems": probs, "short": short,
                     "same_frames": a["frames_sha256"] == b["frames_sha256"]})
    for a in cs_alone:
        rows.append({"start": a["start_sample"], "cls": "C# lock only", "cs": a, "cxx": None,
                     "problems": [], "short": a["good_frames"], "same_frames": False})
    for b in cxx_alone:
        rows.append({"start": b["start_sample"], "cls": "C++ lock only", "cs": None, "cxx": b,
                     "problems": [], "short": 0, "same_frames": False})
    rows.sort(key=lambda r: r["start"])
    return rows, cs_sum, cxx_sum


def totals(rows):
    t = {"bursts": len(rows), "both": 0, "C# only": 0, "C++ only": 0, "neither": 0,
         "C# lock only": 0, "C++ lock only": 0, "cs_frames": 0, "cxx_frames": 0, "short": 0,
         "problems": 0, "frames_differ": 0, "cs_cpu": 0.0, "cxx_cpu": 0.0}
    for r in rows:
        t[r["cls"]] += 1
        t["cs_frames"] += r["cs"]["good_frames"] if r["cs"] else 0
        t["cxx_frames"] += r["cxx"]["good_frames"] if r["cxx"] else 0
        t["short"] += r["short"] if r["cs"] and r["cxx"] else 0
        t["problems"] += len(r["problems"])
        if r["cs"] and r["cxx"] and r["cs"]["good_frames"] and not r["same_frames"] and r["cs"]["good_frames"] == r["cxx"]["good_frames"]:
            t["frames_differ"] += 1
        t["cs_cpu"] += r["cs"]["cpu_ms"] if r["cs"] else 0
        t["cxx_cpu"] += r["cxx"]["cpu_ms"] if r["cxx"] else 0
    return t


def budget_problem(t):
    """The corpus-wide part of the rule: at most 2 percent of the C#'s good frames short."""
    paired_cs = t["cs_frames"]
    return t["short"] > paired_cs * 2 // 100


def clock(slot_start, sample):
    if slot_start is None:
        return f"{sample / RATE:7.1f}s"
    return (slot_start + timedelta(seconds=sample / RATE)).strftime("%H:%M:%S")


def slot_start_of(name):
    try:
        return datetime.strptime(name[:16], "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def ms(x):
    return f"{x / 1000:.1f}" if x is not None else "-"


def fmt_cpu_ratio(cs, cxx):
    return f"{cs / cxx:.1f}x" if cxx else "-"


def slot_report(rows, cs_sum, cxx_sum, title, slot_start):
    t = totals(rows)
    L = [f"# {title}", ""]
    L.append(f"{t['bursts']} bursts: {t['both']} decoded by both, {t['C# only']} by C# only, {t['C++ only']} by C++ only, "
             f"{t['neither']} by neither, {t['C# lock only']} locked by C# only, {t['C++ lock only']} locked by C++ only. "
             f"Good IL2P frames: C# {t['cs_frames']}, C++ {t['cxx_frames']}. "
             f"Layer 4 rule problems: {t['problems']}" + (" (and the 2 percent frame budget is exceeded)" if budget_problem(t) else "") + ".")
    L.append("")
    audio = cs_sum.get("audio_s") or cxx_sum.get("audio_s") or 0
    L.append(f"Processor time over {audio:.0f} s of audio: C# {ms(cs_sum.get('cpu_ms_total'))} s "
             f"({ms(cs_sum.get('cpu_ms_in_bursts'))} s in bursts), C++ {ms(cxx_sum.get('cpu_ms_total'))} s "
             f"({ms(cxx_sum.get('cpu_ms_in_bursts'))} s in bursts). Decoder versions: C# {cs_sum.get('version', '?')}, C++ {cxx_sum.get('version', '?')}.")
    L.append("")
    L.append("| # | UTC | WN | IL | result | frames C# | frames C++ | end C# | end C++ | blocks C#/C++ | rule | cpu C# s | cpu C++ s |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for n, r in enumerate(rows):
        a, b = r["cs"], r["cxx"]
        ref = a or b
        verdict = "ok" if not r["problems"] and r["cls"] not in ("C# lock only", "C++ lock only") else ("; ".join(r["problems"]) or "n/a")
        if r["cls"] == "both" and not r["same_frames"] and a["good_frames"] == b["good_frames"]:
            verdict += " (frames differ)"
        L.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            n, clock(slot_start, r["start"]), ref["wn"], ref["il"], r["cls"],
            a["good_frames"] if a else "-", b["good_frames"] if b else "-",
            a["reason"] if a else "-", b["reason"] if b else "-",
            f"{a['blocks']}/{b['blocks']}" if a and b else "-", verdict,
            ms(a["cpu_ms"]) if a else "-", ms(b["cpu_ms"]) if b else "-"))
    L.append("")
    return "\n".join(L), t


def unexplained(rows):
    """Rows needing a look: one-sided decodes, rule problems, locks on one side only with good frames."""
    out = []
    for r in rows:
        why = []
        if r["cls"] in ("C# only", "C++ only"):
            why.append(r["cls"] + " decode")
        if r["cls"] == "C# lock only" and r["cs"]["good_frames"] > 0:
            why.append("C++ missed a burst with good frames")
        if r["cls"] == "C++ lock only" and r["cxx"]["good_frames"] > 0:
            why.append("C# missed a burst with good frames")
        why += r["problems"]
        if why:
            out.append((r, why))
    return out


def daily_report(slots_dir, date):
    day = date.replace("-", "")
    names = sorted(n for n in os.listdir(slots_dir) if n.startswith(day) and
                   os.path.exists(os.path.join(slots_dir, n, "cxx.jsonl")) and os.path.exists(os.path.join(slots_dir, n, "cs.jsonl")))
    iso = f"{day[:4]}-{day[4:6]}-{day[6:8]}"
    L = [f"# Shadow receiver, {iso}", ""]
    if not names:
        L.append("No slot with both decodes on this day.")
        return "\n".join(L) + "\n"
    allrows, per_slot, flagged = [], [], []
    cs_total = cxx_total = audio_total = cs_burst = cxx_burst = 0.0
    versions = {}
    for n in names:
        rows, cs_sum, cxx_sum = analyse(read_events(os.path.join(slots_dir, n, "cs.jsonl")),
                                        read_events(os.path.join(slots_dir, n, "cxx.jsonl")))
        t = totals(rows)
        per_slot.append((n, t, cs_sum, cxx_sum))
        allrows += rows
        st = slot_start_of(n)
        for r, why in unexplained(rows):
            flagged.append((n, clock(st, r["start"]), r, why))
        cs_total += cs_sum.get("cpu_ms_total", 0); cxx_total += cxx_sum.get("cpu_ms_total", 0)
        cs_burst += cs_sum.get("cpu_ms_in_bursts", 0); cxx_burst += cxx_sum.get("cpu_ms_in_bursts", 0)
        audio_total += cs_sum.get("audio_s", 0)
        versions = {"cs": cs_sum.get("version", "?"), "cxx": cxx_sum.get("version", "?")}
    t = totals(allrows)
    L.append(f"{len(names)} slots, {t['bursts']} bursts: {t['both']} decoded by both decoders, {t['C# only']} by C# only, "
             f"{t['C++ only']} by C++ only, {t['neither']} by neither; {t['C# lock only']} locked by C# only, {t['C++ lock only']} by C++ only.")
    L.append("")
    L.append(f"Good IL2P frames: C# {t['cs_frames']}, C++ {t['cxx_frames']} (C++ short of the C# on paired bursts: {t['short']}"
             + (", over the 2 percent budget" if budget_problem(t) else ", within the 2 percent budget") + "). "
             f"Layer 4 rule problems: {t['problems']}. Differences to look at: {len(flagged)}.")
    L.append("")
    L.append(f"Processor time over {audio_total / 3600:.2f} h of audio: C# {cs_total / 1000:.0f} s ({cs_burst / 1000:.0f} s in bursts), "
             f"C++ {cxx_total / 1000:.0f} s ({cxx_burst / 1000:.0f} s in bursts)"
             + (f", the C# using {cs_total / cxx_total:.1f} times the C++'s" if cxx_total else "") + ". "
             f"Versions: C# {versions['cs']}, C++ {versions['cxx']}. The two run on the same host one after the other, so the ratio is what "
             "matters, not the absolute numbers; the C# figure includes JIT warm-up.")
    L.append("")
    L.append("| slot | bursts | both | C# only | C++ only | neither | one-sided lock | frames C# | frames C++ | rule problems | cpu C# s | cpu C++ s |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for n, tt, cs_sum, cxx_sum in per_slot:
        L.append(f"| {n} | {tt['bursts']} | {tt['both']} | {tt['C# only']} | {tt['C++ only']} | {tt['neither']} | "
                 f"{tt['C# lock only'] + tt['C++ lock only']} | {tt['cs_frames']} | {tt['cxx_frames']} | {tt['problems']} | "
                 f"{ms(cs_sum.get('cpu_ms_total'))} | {ms(cxx_sum.get('cpu_ms_total'))} |")
    L.append("")
    by_wn = {}
    for r in allrows:
        ref = r["cs"] or r["cxx"]
        by_wn.setdefault(ref["wn"], []).append(r)
    L.append("| waveform | bursts | both | C# only | C++ only | neither | frames C# | frames C++ |")
    L.append("|---|---|---|---|---|---|---|---|")
    for wn in sorted(by_wn):
        tt = totals(by_wn[wn])
        L.append(f"| WN {wn} | {tt['bursts']} | {tt['both']} | {tt['C# only']} | {tt['C++ only']} | {tt['neither']} | {tt['cs_frames']} | {tt['cxx_frames']} |")
    L.append("")
    if flagged:
        L.append("## Differences to look at")
        L.append("")
        for n, when, r, why in flagged:
            a, b = r["cs"], r["cxx"]
            L.append(f"- {n} {when}: {'; '.join(why)} (frames C# {a['good_frames'] if a else '-'}, C++ {b['good_frames'] if b else '-'}; "
                     f"end {a['reason'] if a else '-'} / {b['reason'] if b else '-'})")
        L.append("")
    else:
        L.append("No differences to look at: every burst either decoded the same frames or neither decoder had a good frame.")
        L.append("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("slot", nargs="?", help="slot directory holding cxx.jsonl and cs.jsonl")
    ap.add_argument("--cxx"); ap.add_argument("--cs")
    ap.add_argument("--daily", nargs=2, metavar=("SLOTS_DIR", "DATE"))
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.daily:
        text = daily_report(*a.daily)
    else:
        if a.slot:
            cxx_path, cs_path = os.path.join(a.slot, "cxx.jsonl"), os.path.join(a.slot, "cs.jsonl")
            name = os.path.basename(os.path.normpath(a.slot))
        elif a.cxx and a.cs:
            cxx_path, cs_path, name = a.cxx, a.cs, os.path.basename(os.path.dirname(os.path.abspath(a.cxx)))
        else:
            ap.error("give a slot directory, or --cxx and --cs, or --daily")
        rows, cs_sum, cxx_sum = analyse(read_events(cs_path), read_events(cxx_path))
        text, _ = slot_report(rows, cs_sum, cxx_sum, f"Shadow receiver, slot {name}", slot_start_of(name))
        text += "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
