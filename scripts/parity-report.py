# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes the parity report for scripts/parity-check.sh (docs/dev/parity.md) and the STATUS file."""
import argparse
import json
import os
import re
import urllib.request
import xml.etree.ElementTree as ET

# Which stage a C++ test belongs to, by its name; the first match wins.
STAGES = [
    ("Preamble detector", r"preamble detector"),
    ("WN 0 receiver", r"wn ?0"),
    ("Equaliser", r"^(reference )?equaliser"),
    ("Equaliser receive path", r"demodulator|dfe|turbo|mfb"),
    ("Public receiver (C++ and C)", r"receiver|receive through"),
    ("Bits on the wire and transmit", r"table|framing|fec|soft|layer 1|transmit|modulat|scrambl|interleav|probe|preamble|burst"),
]
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def stage_of(name):
    low = name.lower()
    for stage, pattern in STAGES:
        if re.search(pattern, low):
            return stage
    return "Other"


def ctest_results(path):
    if not os.path.exists(path):
        return []
    out = []
    for case in ET.parse(path).getroot().iter("testcase"):
        status = case.get("status", "run")
        if case.find("failure") is not None or status == "fail":
            status = "fail"
        elif case.find("skipped") is not None or status in ("notrun", "disabled"):
            status = "skip"
        else:
            status = "pass"
        out.append((case.get("name", "?"), status))
    return out


def interop(path):
    """(ran, failed tests, summary lines) from an xunit log."""
    if not os.path.exists(path):
        return False, ["no log"], []
    text = ANSI.sub("", open(path, encoding="utf-8", errors="replace").read())
    if re.search(r"Total: 0\s*$", text, re.M):
        return False, [], ["(no such tests in this checkout)"]
    total = re.search(r"Total: (\d+), Errors: (\d+), Failed: (\d+)", text)
    failed = sorted({m.group(1) for m in re.finditer(r"^\s*(.+?) \[FAIL\]\s*$", text, re.M)})
    if total is None:
        failed = failed or ["did not run: " + (text.strip().splitlines() or ["empty log"])[-1]]
    elif int(total.group(1)) == 0:
        return False, [], ["(no such tests in this checkout)"]
    elif (int(total.group(2)) > 0 or int(total.group(3)) > 0) and not failed:
        failed = [f"{total.group(3)} failed, {total.group(2)} errors"]
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith(("layer 4,", "differential:")) and line not in lines:
            lines.append(line)
    return total is not None, failed, lines


def csharp_changes(old, new):
    url = f"https://api.github.com/repos/M0LTE/M0LTE.Ms110d/compare/v{old}...v{new}"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    token = os.environ.get("GH_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        data = json.load(urllib.request.urlopen(req, timeout=30))
    except Exception as e:  # the report still says what it can
        return [f"(could not list the C# changes: {e})"], []
    commits = [c["commit"]["message"].splitlines()[0] for c in data.get("commits", [])]
    files = [f["filename"] for f in data.get("files", []) if f["filename"].startswith("src/")]
    return commits, files


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", required=True)
    p.add_argument("--old", required=True)
    p.add_argument("--new", required=True)
    p.add_argument("--work", required=True)
    p.add_argument("--out", required=True)
    a = p.parse_args()

    md = []
    gap = False
    if a.mode == "new":
        md.append(f"# C# parity: {a.old} to {a.new}\n")
        md.append(f"The pin is C# {a.old}; the newest release is {a.new}. Everything below ran against {a.new}.\n")
        commits, files = csharp_changes(a.old, a.new)
        md.append(f"## C# changes since {a.old}\n")
        md += [f"- {c}" for c in commits] or ["- (none listed)"]
        if files:
            md.append("\nSource files changed: " + ", ".join(f"`{f}`" for f in files) + "\n")
        md.append("\n## C++ tests against the new C#\n")
        results = ctest_results(os.path.join(a.work, "ctest.xml"))
        if not results:
            md.append("The C++ tests did not run (no results); see the job log.\n")
            gap = True
        else:
            md.append("| Stage | Tests | Pass | Fail | Skipped |")
            md.append("| --- | --- | --- | --- | --- |")
            order = [s for s, _ in STAGES] + ["Other"]
            for stage in order:
                rows = [s for n, s in results if stage_of(n) == stage]
                if rows:
                    md.append(f"| {stage} | {len(rows)} | {rows.count('pass')} | {rows.count('fail')} | {rows.count('skip')} |")
            failing = [n for n, s in results if s == "fail"]
            if failing:
                gap = True
                md.append("\nFailing:\n")
                md += [f"- {stage_of(n)}: {n}" for n in failing[:40]]
                if len(failing) > 40:
                    md.append(f"- and {len(failing) - 40} more")
        md.append("\n## Interop tests (C# and C++ side by side)\n")
        names = [("Acquisition scenarios", "acquisition.log"), ("Real audio, layer 4", "corpus.log")]
    else:
        md.append(f"# C# parity: pin {a.old} is the newest release\n")
        names = []
    names.append(("Random streams", "differential.log"))
    for title, log in names:
        ran, failed, lines = interop(os.path.join(a.work, log))
        verdict = "pass" if ran and not failed else ("not run" if not failed else "FAIL")
        md.append(f"- **{title}: {verdict}**")
        md += [f"  - {line}" for line in lines[:9]]
        if len(lines) > 9:
            md.append(f"  - and {len(lines) - 9} more lines in the job's artifact")
        md += [f"  - failed: {f}" for f in failed]
        if failed or not ran:  # a class that did not run cannot vouch for parity
            gap = True
    md.append("\nNot run here: the performance masks (masks.yml) and layer 3.\n")

    if a.mode == "new" and not gap:
        status = "nogap"
        md.insert(1, f"**No gap.** The C++ gives the C# {a.new} results on every test run. Bump the pin "
                     "(reference/pin.txt, the tool project, the dumps; docs/dev/reference-files.md).\n")
    elif gap:
        status = "gap"
        md.insert(1, f"**Gap.** The C++ differs from the C# {a.new} where marked below. "
                     "Each failing test names the stage; random-stream failures name a seed that replays the stream.\n")
    else:
        status = "current"
        md.insert(1, "**Nothing to do.** The random streams decode the same on both receivers.\n")
    open(a.out, "w", encoding="utf-8").write("\n".join(md) + "\n")
    open(os.path.join(a.work, "STATUS"), "w").write(status + "\n")


if __name__ == "__main__":
    main()
