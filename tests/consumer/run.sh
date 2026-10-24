#!/bin/sh
# Builds and runs both consumer projects against this checkout.
# Usage: tests/consumer/run.sh <work-dir> [extra cmake args for the library, e.g. -DMS110D_PRECISION=single]
set -eu
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$1
shift
rm -rf "$work"
mkdir -p "$work"
work=$(cd "$work" && pwd)

echo "== FetchContent consumer"
cmake -S "$src/tests/consumer/fetchcontent" -B "$work/fc" "-DMS110D_SOURCE_DIR=$src" "$@"
cmake --build "$work/fc" -j4
"$work/fc/consumer"

echo "== find_package consumer"
cmake -S "$src" -B "$work/lib" -DMS110D_BUILD_TESTS=OFF "$@"
cmake --build "$work/lib" -j4
cmake --install "$work/lib" --prefix "$work/prefix"
cmake -S "$src/tests/consumer/findpackage" -B "$work/fp" "-DCMAKE_PREFIX_PATH=$work/prefix"
cmake --build "$work/fp" -j4
"$work/fp/consumer"
