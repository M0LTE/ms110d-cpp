#!/bin/sh
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# How far is this repo from the newest C# release? docs/dev/parity.md.
#
# Usage: scripts/parity-check.sh --modify-checkout [--version X.Y.Z] REPORT.md
#
# With a C# release newer than reference/pin.txt (the newest on NuGet, or --version), it moves the
# pin in THIS checkout to it, regenerates every C# reference file and dump from it, builds and runs
# the C++ tests and the interop tests against it (masks and layer 3 aside) and writes a report: the
# C# changes since the pin and, stage by stage, which tests now fail. Nothing is committed; use a
# throwaway checkout (CI) or a worktree, and throw it away afterwards. With the pin current it only
# runs the random-stream test with fresh seeds. Either way it then writes STATUS (next to the
# report): "current" (pin current, streams equal), "nogap" (newer C#, nothing differs: bump the
# pin), or "gap". Needs git, curl, python3, cmake, a C++ compiler and a .NET 10 SDK.
set -eu

modify=0
version=
report=
while [ $# -gt 0 ]; do
  case $1 in
    --modify-checkout) modify=1; shift ;;
    --version) version=$2; shift 2 ;;
    *) report=$1; shift ;;
  esac
done
[ "$modify" = 1 ] && [ -n "$report" ] || { echo "usage: $0 --modify-checkout [--version X.Y.Z] REPORT.md" >&2; exit 2; }

src=$(cd "$(dirname "$0")/.." && pwd)
cd "$src"
mkdir -p "$(dirname "$report")"
report=$(cd "$(dirname "$report")" && pwd)/$(basename "$report")
work=$src/build/parity
mkdir -p "$work"
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1

old=$(sed -n 's/^nuget_version=//p' reference/pin.txt)
if [ -z "$version" ]; then
  version=$(curl -fsS https://api.nuget.org/v3-flatcontainer/m0lte.ms110d/index.json |
    python3 -c 'import json,sys; v=[x for x in json.load(sys.stdin)["versions"] if "-" not in x]; print(max(v, key=lambda s: tuple(map(int, s.split(".")))))')
fi
streams=${MS110D_DIFF_STREAMS:-300}
seed=${MS110D_DIFF_SEED:-$(date -u +%Y%j)}
echo "pin $old, C# $version, random streams $streams from seed $seed"

mode=current
if [ "$version" != "$old" ]; then
  mode=new
  # The commit: an annotated tag's peeled line (^{}) when there is one, else the tag itself.
  tags=$(git ls-remote https://github.com/M0LTE/M0LTE.Ms110d "refs/tags/v$version" "refs/tags/v$version^{}")
  sha=$(echo "$tags" | awk '/\^\{\}$/{print $1}' | head -1)
  [ -n "$sha" ] || sha=$(echo "$tags" | awk 'NR==1{print $1}')
  [ -n "$sha" ] || { echo "no tag v$version in M0LTE/M0LTE.Ms110d" >&2; exit 1; }
  sed -i "s/^nuget_version=.*/nuget_version=$version/; s/^git_ref=.*/git_ref=v$version/; s/^git_sha=.*/git_sha=$sha/" reference/pin.txt
  sed -i "s/Include=\"M0LTE.Ms110d\" Version=\"\[[^]]*\]\"/Include=\"M0LTE.Ms110d\" Version=\"[$version]\"/" tools/csharp-vectors/CsharpVectors.csproj
  dotnet restore --force --no-cache tools/csharp-vectors
  dotnet restore --force --no-cache tests/interop

  # Every C# file the tests read, from the new release, under the names the tests are built with.
  scripts/generate-reference.sh --force
  ref=$src/build/_reference/ref
  tool="dotnet run -c Release --project tools/csharp-vectors --"
  dump() { sed -n "s|.*data/\(csharp-$1-[0-9.]*\.txt\).*|tests/data/\1|p" tests/CMakeLists.txt | head -1; }
  for m in tables framing soft; do $tool $m > "$(dump $m)"; done
  for m in dfe detect wn0 wn0-awgn; do $tool $m "$ref" > "$(dump $m)"; done
  $tool > "$work/vectors.txt"
  python3 tools/csharp-vectors/make_header.py "$work/vectors.txt" > tests/csharp_vectors.hpp
  $tool fec > "$work/fec.txt"
  python3 tools/csharp-vectors/make_fec_inc.py "$work/fec.txt" > tests/fec_vectors.inc
  scripts/generate-rx-dumps.sh --force
  scripts/generate-wn0-poor.sh --force
  scripts/fetch-corpus.sh
fi

cmake -S . -B build/parity/build -DCMAKE_BUILD_TYPE=Release -DMS110D_DOTNET_TESTS=OFF \
  -DMS110D_REFERENCE_DIR="$src/build/_reference/ref" -DMS110D_WN0_POOR_STEM="$src/build/_reference/wn0-poor" \
  -DMS110D_RX_DUMP_DIR="$src/build/_reference" >/dev/null
if [ "$mode" = new ]; then
  cmake --build build/parity/build -j"$(nproc)" >/dev/null
else
  cmake --build build/parity/build -j"$(nproc)" --target ms110d_c >/dev/null  # the random streams need only this
fi

export MS110D_PIN_FILE=$src/reference/pin.txt MS110D_NATIVE_LIB=$src/build/parity/build/libms110d_c.so MS110D_EXPECTED_VERSION=0
interop() {  # class, log
  dotnet run --project tests/interop -c Release -- -class "Ms110dInterop.Tests.$1" > "$work/$2" 2>&1 || true
}
rm -f "$work"/*.log "$work"/ctest.xml
if [ "$mode" = new ]; then
  MS110D_REQUIRE_REFERENCE=1 ctest --test-dir build/parity/build -E "masks|dotnet" -j"$(nproc)" \
    --output-junit "$work/ctest.xml" > "$work/ctest.log" 2>&1 || true
  interop AcquisitionTests acquisition.log
  MS110D_REQUIRE_CORPUS=1 MS110D_CORPUS_DIR=$src/build/_corpus/current interop CorpusTests corpus.log
fi
MS110D_DIFF_STREAMS=$streams MS110D_DIFF_SEED=$seed interop DifferentialTests differential.log

python3 scripts/parity-report.py --mode "$mode" --old "$old" --new "$version" --work "$work" --out "$report"
cat "$work/STATUS" > "$(dirname "$report")/STATUS"
echo "status $(cat "$work/STATUS"); report in $report"
