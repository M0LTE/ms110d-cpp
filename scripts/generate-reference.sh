#!/bin/sh
# Generates the C# reference files this repo's tests compare against.
#
# Usage: scripts/generate-reference.sh [--force] [cache-dir]
#
# Reads reference/pin.txt, clones M0LTE/M0LTE.Ms110d at git_ref into <cache-dir>/csharp,
# runs RefGen's `standard` set (it generates twice and checks the two trees match), then
# hashes the result. The files land in <cache-dir>/ref. The default cache-dir is
# build/_reference, which is git-ignored; the files are about 40 MiB and are never committed.
# A stamp file makes a rerun with an unchanged pin a no-op; --force regenerates anyway.
# Needs git and a .NET 10 SDK. See docs/dev/reference-files.md.
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

pin() {
  sed -n "s/^$1=//p" "$src/reference/pin.txt" | head -n 1 | tr -d '\r '
}
git_ref=$(pin git_ref)
nuget_version=$(pin nuget_version)
git_sha=$(pin git_sha)
if [ -z "$git_ref" ] || [ -z "$nuget_version" ] || [ -z "$git_sha" ]; then
  echo "reference/pin.txt must define nuget_version, git_ref and git_sha" >&2
  exit 1
fi
if [ "$git_ref" != "v$nuget_version" ]; then
  echo "pin mismatch: git_ref is $git_ref but nuget_version $nuget_version needs v$nuget_version" >&2
  exit 1
fi

stamp="$cache/stamp"
want="git_ref=$git_ref git_sha=$git_sha"
refgen="$cache/csharp/tools/M0LTE.Ms110d.RefGen"
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(sed -n 1p "$stamp")" = "$want" ] && [ -d "$cache/ref" ] && [ -d "$refgen" ]; then
  # Do not trust the stamp alone: recompute the tree hash and compare.
  expected=$(sed -n 2p "$stamp" | sed 's/^tree_hash=//')
  actual=$(dotnet run -c Release --project "$refgen" -- hash "$cache/ref" | tail -n 1 | awk '{print $1}')
  if [ "$actual" = "$expected" ]; then
    echo "Reference files for $git_ref are in $cache/ref and the tree hash still matches"
    echo "tree_hash=$actual"
    exit 0
  fi
  echo "Cached reference files do not match their recorded hash; regenerating"
fi

rm -rf "$cache/csharp" "$cache/ref" "$stamp"
echo "== Fetching M0LTE/M0LTE.Ms110d at $git_ref"
git init -q "$cache/csharp"
git -C "$cache/csharp" remote add origin https://github.com/M0LTE/M0LTE.Ms110d.git
git -C "$cache/csharp" fetch -q --depth 1 origin "$git_ref"
git -C "$cache/csharp" checkout -q FETCH_HEAD
got=$(git -C "$cache/csharp" rev-parse HEAD)
if [ "$got" != "$git_sha" ]; then
  echo "tag $git_ref resolves to $got but reference/pin.txt says git_sha=$git_sha" >&2
  exit 1
fi

echo "== Running RefGen standard (generates twice and verifies)"
# RefGen looks at the git checkout of the current directory, so pin the commit explicitly.
dotnet run -c Release --project "$refgen" -- standard --out "$cache/ref" --verify --git-commit "$got"

echo "== Hashing"
hash_out=$(dotnet run -c Release --no-build --project "$refgen" -- hash "$cache/ref")
echo "$hash_out"
hash=$(echo "$hash_out" | tail -n 1 | awk '{print $1}')

pkg=$(sed -n 's/^package_version=//p' "$cache/ref/index.txt" | head -n 1)
echo "RefGen package_version in the files: $pkg (pin.txt nuget_version: $nuget_version)"
printf '%s\ntree_hash=%s\n' "$want" "$hash" > "$stamp"
echo "Reference files for $git_ref are in $cache/ref"
echo "tree_hash=$hash"
