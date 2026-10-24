#!/bin/sh
# Fetches the layer 4 real-audio corpus (about 13 MB of 9600 Hz audio), checks every hash and
# unpacks it outside the source tree.
#
# Usage: scripts/fetch-corpus.sh [--force] [--from FILE] [cache-dir]
#
# Reads reference/corpus.txt, downloads the release asset ms110d-<corpus_tag>.tar.gz from
# the GitHub release of the same name, checks the tarball against tarball_sha256, unpacks it to
# <cache-dir>/current and checks each burst's file against the hash in manifest.json. The default
# cache-dir is build/_corpus (git-ignored). A rerun with an unchanged pin re-checks the burst
# hashes and does nothing else; --force fetches again. --from FILE uses a local tarball instead of
# downloading (for a package you have just made). Needs curl, tar, sha256sum and python3.
# See docs/dev/corpus.md.
set -eu

force=0
from=
while [ $# -gt 0 ]; do
  case $1 in
    --force) force=1; shift ;;
    --from) from=$2; shift 2 ;;
    *) break ;;
  esac
done

src=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$src/build/_corpus}
mkdir -p "$cache"
cache=$(cd "$cache" && pwd)

pin() {
  sed -n "s/^$1=//p" "$src/reference/corpus.txt" | head -n 1 | tr -d '\r '
}
tag=$(pin corpus_tag)
want=$(pin tarball_sha256)
if [ -z "$tag" ] || [ -z "$want" ]; then
  echo "reference/corpus.txt must define corpus_tag and tarball_sha256" >&2
  exit 1
fi
if [ "$want" = "PENDING" ]; then
  echo "reference/corpus.txt has no tarball_sha256 yet: the release $tag has not been published" >&2
  exit 1
fi

cur="$cache/current"
stamp="$cache/stamp"

# Checks every burst file against manifest.json; prints the number checked.
verify() {
  python3 -I - "$1" <<'PY'
import hashlib, json, os, sys
d = sys.argv[1]
m = json.load(open(os.path.join(d, "manifest.json")))
for b in m["bursts"]:
    h = hashlib.sha256(open(os.path.join(d, b["file"]), "rb").read()).hexdigest()
    if h != b["sha256"]:
        sys.exit("hash mismatch: " + b["file"])
print(len(m["bursts"]), "bursts verified")
PY
}

if [ "$force" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "corpus_tag=$tag tarball_sha256=$want" ] && [ -f "$cur/manifest.json" ]; then
  verify "$cur"
  echo "Corpus $tag is in $cur"
  exit 0
fi

rm -rf "$cur" "$stamp"
tarball="$cache/ms110d-$tag.tar.gz"
if [ -n "$from" ]; then
  cp "$from" "$tarball"
else
  echo "== Downloading $tag"
  curl -fL --retry 3 -o "$tarball" "https://github.com/M0LTE/ms110d-cpp/releases/download/$tag/ms110d-$tag.tar.gz"
fi
got=$(sha256sum "$tarball" | awk '{print $1}')
if [ "$got" != "$want" ]; then
  rm -f "$tarball"
  echo "tarball hash $got does not match reference/corpus.txt ($want)" >&2
  exit 1
fi
mkdir -p "$cur"
tar -xzf "$tarball" -C "$cur"
verify "$cur"
printf 'corpus_tag=%s tarball_sha256=%s' "$tag" "$want" > "$stamp"
echo "Corpus $tag is in $cur"
