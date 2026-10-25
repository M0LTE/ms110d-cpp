#!/bin/sh
# Generates the performance mask audio the mask tests decode (docs/dev/masks.md): for each point of Table
# D-LXIV (AWGN and Poor channel, 3 kHz) and the D-LXV three path static channel, bursts through M0LTE.HfChannel
# and what the C# 0.2.1 demodulator does with them. Made by tools/csharp-vectors (masks mode), never committed.
# Usage: scripts/generate-masks.sh [--force] [--profile ci|long] [cache-dir] [point-id ...]
# The files land in <cache-dir>/masks-<profile>/<id>.{txt,i16le,bits}; the default cache-dir is build/_reference and
# the default profile is ci (370,000 bits per point, 640 to 1300 s of audio, about 350 MiB in all). The long profile is the
# full 3,000,000 bits per point (600 s for Poor) and takes hours; give it point ids to make one at a time.
# A stamp of the tool sources, the pin and this script makes a rerun a no-op (not with point ids).
# MS110D_MASK_THREADS sets the bursts made at once (default 2).
set -eu

force=0
profile=ci
while [ $# -gt 0 ]; do
  case "$1" in
    --force) force=1; shift ;;
    --profile) profile=$2; shift 2 ;;
    *) break ;;
  esac
done

src=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$src/build/_reference}
[ $# -gt 0 ] && shift
dir="$cache/masks-$profile"
mkdir -p "$dir"
dir=$(cd "$dir" && pwd)

want=$(cat "$src/reference/pin.txt" "$src"/tools/csharp-vectors/*.cs "$src"/tools/csharp-vectors/*.csproj "$0" | sha256sum | awk '{print $1}')
stamp="$dir/masks.stamp"
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1

if [ $# -gt 0 ]; then
  dotnet run -c Release --project "$src/tools/csharp-vectors" -- masks write "$profile" "$dir" "$@"
  exit 0
fi

complete=1
for id in $(dotnet run -c Release --project "$src/tools/csharp-vectors" -- masks list "$profile"); do
  [ -f "$dir/$id.txt" ] && [ -f "$dir/$id.i16le" ] && [ -f "$dir/$id.bits" ] || complete=0
done
if [ "$force" = 0 ] && [ "$complete" = 1 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
  echo "Mask audio ($profile) up to date in $dir"
  exit 0
fi

rm -f "$stamp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- masks write "$profile" "$dir"
echo "$want" > "$stamp"
echo "Mask audio ($profile) written to $dir"
