#!/bin/sh
# Regenerates every committed dump of the C# reference and fails if any differs from the file in
# the repo: tests/data/csharp-{tables,framing,dfe,soft}-0.2.1.txt, tests/csharp_vectors.hpp and
# tests/fec_vectors.inc. The dumps are made by tools/csharp-vectors from the pinned NuGet package,
# so a dump that no longer matches means the tool, the pin or the committed file is out of step.
#
# Usage: scripts/check-dumps.sh [reference-dir]
# The reference directory (default build/_reference/ref, from scripts/generate-reference.sh) supplies
# the real symbol streams of the dfe dump. Needs a .NET 10 SDK and python3.
set -eu

src=$(cd "$(dirname "$0")/.." && pwd)
ref=${1:-$src/build/_reference/ref}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
tool="dotnet run -c Release --project $src/tools/csharp-vectors --"

cd "$src"
$tool tables > "$tmp/tables.txt"
$tool framing > "$tmp/framing.txt"
$tool dfe "$ref" > "$tmp/dfe.txt"
$tool soft > "$tmp/soft.txt"
$tool > "$tmp/vectors.txt"
python3 tools/csharp-vectors/make_header.py "$tmp/vectors.txt" > "$tmp/csharp_vectors.hpp"
$tool fec > "$tmp/fec.txt"
python3 tools/csharp-vectors/make_fec_inc.py "$tmp/fec.txt" > "$tmp/fec_vectors.inc"

status=0
check() {
  if cmp -s "$1" "$2"; then
    echo "ok       $2"
  else
    echo "DIFFERS  $2" >&2
    diff -u "$2" "$1" | head -20 >&2 || true
    status=1
  fi
}
check "$tmp/tables.txt" tests/data/csharp-tables-0.2.1.txt
check "$tmp/framing.txt" tests/data/csharp-framing-0.2.1.txt
check "$tmp/dfe.txt" tests/data/csharp-dfe-0.2.1.txt
check "$tmp/soft.txt" tests/data/csharp-soft-0.2.1.txt
check "$tmp/csharp_vectors.hpp" tests/csharp_vectors.hpp
check "$tmp/fec_vectors.inc" tests/fec_vectors.inc
exit $status
