#!/bin/sh
# Generates the Poor channel WN 0 corpus the receiver tests compare against: bursts through
# M0LTE.HfChannel's Poor channel, their audio as 16 bit samples, and what the C# 0.3.0
# demodulator does with them. Made by tools/csharp-vectors (wn0-poor mode), never committed
# (about 3 MiB). Usage: scripts/generate-wn0-poor.sh [--force] [cache-dir]
# The files land in <cache-dir>/wn0-poor.txt and wn0-poor.i16le; the default cache-dir is
# build/_reference. A stamp of the tool sources and the pin makes a rerun a no-op.
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
stamp="$cache/wn0-poor.stamp"
if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && [ -f "$cache/wn0-poor.txt" ] && [ -f "$cache/wn0-poor.i16le" ]; then
  echo "Poor channel corpus up to date in $cache"
  exit 0
fi

export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
rm -f "$stamp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- wn0-poor "$cache/wn0-poor"
echo "$want" > "$stamp"
echo "Poor channel corpus written to $cache/wn0-poor.txt and .i16le"
