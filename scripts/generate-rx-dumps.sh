#!/bin/sh
# Generates the C# dumps the equaliser receive path's tests compare against, with tools/csharp-vectors,
# never committed: what the C# 0.2.1 demodulator does with the WN 1 to 5 ultra-short and short
# reference cases (rx) and with seeded noisy and cut bursts (rx-awgn), both turbo off; the shipped
# receiver on every WN 1 to 8 and 13 reference case (rx-turbo); and the corpus of Poor, Moderate and
# shifted multipath bursts and standard probe pattern bursts with their audio (rx-corpus, about 8 MiB);
# and the receiver with each of its instrument options switched on, over part of that corpus and cut
# bursts (rx-options, with the cut bursts' audio, about 15 MiB).
# Usage: scripts/generate-rx-dumps.sh [--force] [cache-dir]
# Needs the reference files in <cache-dir>/ref (scripts/generate-reference.sh). The files land in
# <cache-dir>: csharp-rx.txt, csharp-rx-awgn.txt, csharp-rx-turbo.txt, rx-corpus.txt and
# rx-corpus.i16le, rx-options.txt and rx-options.f32le; the default cache-dir is build/_reference. A stamp of the tool sources, the pin
# and this script makes a rerun a no-op.
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
if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && [ -f "$cache/csharp-rx.txt" ] && [ -f "$cache/csharp-rx-awgn.txt" ] && [ -f "$cache/csharp-rx-turbo.txt" ] && [ -f "$cache/rx-corpus.txt" ] && [ -f "$cache/rx-corpus.i16le" ] && [ -f "$cache/rx-options.txt" ] && [ -f "$cache/rx-options.f32le" ]; then
  echo "Receive dumps up to date in $cache"
  exit 0
fi

export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
# The receive dumps run the C# receiver on the reference cases' standard-pattern audio (issue 68).
"$src/scripts/generate-standard-reference.sh" "$cache"
rm -f "$stamp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx "$cache/ref" > "$cache/csharp-rx.txt.tmp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx-awgn "$cache/ref" > "$cache/csharp-rx-awgn.txt.tmp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx-turbo "$cache/ref" > "$cache/csharp-rx-turbo.txt.tmp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx-corpus "$cache/rx-corpus"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- rx-options "$cache/ref" "$cache/rx-corpus" "$cache/rx-options"
mv "$cache/csharp-rx-turbo.txt.tmp" "$cache/csharp-rx-turbo.txt"
mv "$cache/csharp-rx.txt.tmp" "$cache/csharp-rx.txt"
mv "$cache/csharp-rx-awgn.txt.tmp" "$cache/csharp-rx-awgn.txt"
echo "$want" > "$stamp"
echo "Receive dumps and corpus written to $cache"
