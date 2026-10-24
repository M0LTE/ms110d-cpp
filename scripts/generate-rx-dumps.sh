#!/bin/sh
# Generates the two C# dumps the equaliser receive path's tests compare against: what the C# 0.2.1
# demodulator does with the WN 1 to 5 ultra-short and short reference cases (rx) and with seeded
# noisy and cut bursts (rx-awgn). Made by tools/csharp-vectors, never committed (about 1.1 MiB).
# Usage: scripts/generate-rx-dumps.sh [--force] [cache-dir]
# Needs the reference files in <cache-dir>/ref (scripts/generate-reference.sh). The dumps land in
# <cache-dir>/csharp-rx.txt and csharp-rx-awgn.txt; the default cache-dir is build/_reference. A
# stamp of the tool sources, the pin and this script makes a rerun a no-op.
set -eu

force=0
if [ "${1:-}" = "--force" ]; then
  force=1
  shift
fi

src=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$src/build/_reference}
mkdir -p "$cache"
cache=$(cd "$cache" && pwd)

want=$(cat "$src/reference/pin.txt" "$src"/tools/csharp-vectors/*.cs "$src"/tools/csharp-vectors/*.csproj "$0" | sha256sum | awk '{print $1}')
stamp="$cache/rx-dumps.stamp"
if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && [ -f "$cache/csharp-rx.txt" ] && [ -f "$cache/csharp-rx-awgn.txt" ]; then
  echo "Receive dumps up to date in $cache"
  exit 0
fi

export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
rm -f "$stamp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx "$cache/ref" > "$cache/csharp-rx.txt.tmp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx-awgn "$cache/ref" > "$cache/csharp-rx-awgn.txt.tmp"
mv "$cache/csharp-rx.txt.tmp" "$cache/csharp-rx.txt"
mv "$cache/csharp-rx-awgn.txt.tmp" "$cache/csharp-rx-awgn.txt"
echo "$want" > "$stamp"
echo "Receive dumps written to $cache/csharp-rx.txt and csharp-rx-awgn.txt"
