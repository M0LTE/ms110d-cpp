#!/usr/bin/env python3
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Selects a corpus from a full curation (the output of curate.py over many slots). See docs/dev/corpus.md.

  scripts/corpus/select.py FULL_DIR OUT_DIR ERROR_FREE_PER_WN OTHER_PER_WN

Keeps every burst with no good frame or overlapping a dropout, then for each waveform and for error-free and
not error-free separately, that many bursts spaced evenly through the list sorted by SNR estimate. Copies the
chosen audio and writes a manifest cut down to them. Deterministic.
"""
import json, os, shutil, sys

src, dst, n_ef, n_other = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
m = json.load(open(src + "/manifest.json"))
b = m["bursts"]
keep = {e["id"] for e in b if e["good_frames"] == 0 or e["dropout"]}
rest = [e for e in b if e["id"] not in keep]
for error_free in (False, True):
    for wn in sorted({e["waveform"] for e in b}):
        grp = sorted([e for e in rest if e["error_free"] == error_free and e["waveform"] == wn],
                     key=lambda e: (e["snr_db_estimate"], e["id"]))
        n, want = len(grp), min(len(grp), n_ef if error_free else n_other)
        for i in range(want):
            keep.add(grp[int((i + 0.5) * n / want)]["id"])
sel = [e for e in b if e["id"] in keep]
os.makedirs(dst + "/bursts", exist_ok=True)
for e in sel:
    shutil.copy(src + "/" + e["file"], dst + "/" + e["file"])
m["bursts"] = sel
with open(dst + "/manifest.json", "w") as f:
    json.dump(m, f, indent=1, sort_keys=True)
    f.write("\n")
print(f"{len(sel)} bursts, {sum(os.path.getsize(dst + '/' + e['file']) for e in sel) / 1e6:.1f} MB")
