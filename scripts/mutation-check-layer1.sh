#!/usr/bin/env bash
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Proves layer 1 can fail: breaks one stage at a time in a COPY of the tree, builds it, and
# checks that both the stand-alone reference test and the side-by-side .NET test name that
# stage as the first one that differs. Needs a .NET 10 SDK and the reference files
# (scripts/generate-reference.sh). The working tree is never modified.
#
#   scripts/mutation-check-layer1.sh [work-dir]     (default /tmp/layer1-mutation)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="${1:-/tmp/layer1-mutation}"
ref="${MS110D_REFERENCE_DIR:-$root/build/_reference/ref}"
[ -f "$ref/index.txt" ] || { echo "no reference files at $ref" >&2; exit 2; }

# name | file | sed expression | text both tests must contain
mutations=(
  "encode|src/fec/convolutional.hpp|s/0b101110001u/0b101110000u/|encode"
  "puncture|src/puncture.cpp|0,/(spec.keep_t1 >> col)/s//(spec.keep_t2 >> col)/|puncture"
  "interleave|src/interleaver.hpp|0,/out\[loc\] = in\[n\];/s//out[loc] = in[n ^ 1u];/|interleave"
  "scramble-psk|src/scrambler.hpp|s/iterate\(3\);/iterate(2);/|scramble"
  "scramble-trinomial|src/trinomial_scrambler.hpp|s/shifts_per_symbol = 16/shifts_per_symbol = 15/|scramble"
)

fail=0
for m in "${mutations[@]}"; do
  IFS='|' read -r name file expr want <<<"$m"
  if [ -n "${ONLY:-}" ] && [ "$ONLY" != "$name" ]; then continue; fi
  dir="$work/$name"
  rm -rf "$dir"; mkdir -p "$dir"
  (cd "$root" && tar --exclude=./build --exclude=./.git -cf - .) | tar -xf - -C "$dir"
  sed -i -E "$expr" "$dir/$file"
  if cmp -s "$root/$file" "$dir/$file"; then echo "$name: mutation did not change $file" >&2; exit 2; fi
  cmake -S "$dir" -B "$dir/b" -DCMAKE_BUILD_TYPE=Release -DMS110D_DOTNET_TESTS=ON -DMS110D_REFERENCE_DIR="$ref" >/dev/null
  cmake --build "$dir/b" -j4 >/dev/null

  echo "== $name ($file)"
  out1="$(MS110D_REFERENCE_DIR="$ref" "$dir/b/tests/ms110d_tests" "[layer1]" 2>&1 || true)"
  if grep -q "first stage that differs: .*$want" <<<"$out1"; then echo "   stand-alone: names '$want'"; else echo "   stand-alone: DID NOT name '$want'"; fail=1; fi

  out2="$(cd "$dir/b" && MS110D_L1_FRAMES=20 MS110D_NATIVE_LIB="$dir/b/libms110d_c.so" MS110D_TESTHOOKS_LIB="$dir/b/tests/libms110d_testhooks.so" \
    MS110D_PIN_FILE="$dir/reference/pin.txt" MS110D_EXPECTED_VERSION=0.0.0 DOTNET_NOLOGO=1 \
    dotnet run --project "$dir/tests/interop" -c Release --artifacts-path "$dir/b/dotnet" -- -class Ms110dInterop.Tests.Layer1Tests 2>&1 || true)"
  if grep -q "first stage that differs is '$want" <<<"$out2"; then echo "   side-by-side: names '$want'"; else echo "   side-by-side: DID NOT name '$want'"; fail=1; fi
done
[ "$fail" = 0 ] && echo "all mutations caught" || { echo "SOME MUTATIONS NOT CAUGHT"; exit 1; }
