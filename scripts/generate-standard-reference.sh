#!/bin/sh
# Generates the standard-probe-pattern equivalents of the C# reference files' probe values and audio
# (tools/csharp-vectors, mode standard-reference; docs/dev/reference-files.md). Needs the reference
# files from scripts/generate-reference.sh. The output is in <cache-dir>/standard, one directory per
# case. A stamp keyed on the pin, the tool and this script makes a rerun a no-op; --force regenerates.
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
if [ ! -f "$cache/ref/index.txt" ]; then
  echo "no reference files in $cache/ref; run scripts/generate-reference.sh first" >&2
  exit 1
fi

want=$(cat "$src/reference/pin.txt" "$src"/tools/csharp-vectors/*.cs "$src"/tools/csharp-vectors/*.csproj "$0" | sha256sum | awk '{print $1}')
want="$want $(sed -n 's/^tree_hash=//p' "$cache/stamp" 2>/dev/null)"
stamp="$cache/standard.stamp"
if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && [ -d "$cache/standard" ]; then
  echo "Standard-pattern reference files up to date in $cache/standard"
  exit 0
fi

export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
rm -rf "$cache/standard" "$stamp"
dotnet run -c Release --project "$src/tools/csharp-vectors" -- standard-reference "$cache/ref" "$cache/standard"
echo "$want" > "$stamp"
echo "Standard-pattern reference files written to $cache/standard"
