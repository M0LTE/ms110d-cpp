#!/bin/sh
# Packages a curated corpus (the output of scripts/corpus/curate.py) as a release asset.
#
# Usage: scripts/corpus/package.sh <tag> [curated-dir] [out-dir]
#
# Writes <out-dir>/ms110d-<tag>.tar.gz and a .sha256 beside it, and prints the size, the
# number of bursts and the hash to put in reference/corpus.txt. The tarball is reproducible: fixed
# member order, owner and time, and no name or time in the gzip header. Upload it to the GitHub
# release <tag> by hand; nothing here talks to the network. See docs/dev/corpus.md.
set -eu
tag=${1:?usage: package.sh <tag> [curated-dir] [out-dir]}
cache=${MS110D_CORPUS_CACHE:-$HOME/.cache/ms110d-corpus}
cur=${2:-$cache/curated}
out=${3:-$cache/release}
[ -f "$cur/manifest.json" ] || { echo "no manifest.json in $cur" >&2; exit 1; }
mkdir -p "$out"
tarball="$out/ms110d-$tag.tar.gz"
( cd "$cur" && { echo manifest.json; ls bursts | sed 's,^,bursts/,' | LC_ALL=C sort; } |
    tar --create --files-from=- --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00 UTC' --format=ustar |
    gzip -n -9 ) > "$tarball"
sum=$(sha256sum "$tarball" | awk '{print $1}')
printf '%s  ms110d-%s.tar.gz\n' "$sum" "$tag" > "$tarball.sha256"
n=$(ls "$cur/bursts" | wc -l)
echo "tarball:  $tarball"
echo "size:     $(stat -c %s "$tarball") bytes"
echo "bursts:   $n"
echo "sha256:   $sum"
echo "Put corpus_tag=$tag and tarball_sha256=$sum in reference/corpus.txt, then upload the tarball to the release."
