#!/bin/sh
# Builds and runs both consumer projects against this checkout.
# Usage: tests/consumer/run.sh <work-dir> [extra cmake args for the library, e.g. -DMS110D_PRECISION=single]
# The find_package consumer takes its precision from the installed library, so it gets no extra args.
set -eu
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$1
shift
version=$(sed -n 's/^project(m0lte-ms110d VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
rm -rf "$work"
mkdir -p "$work"
work=$(cd "$work" && pwd)

echo "== FetchContent consumer"
cmake -S "$src/tests/consumer/fetchcontent" -B "$work/fc" "-DMS110D_SOURCE_DIR=$src" "-DEXPECTED_VERSION=$version" "$@"
cmake --build "$work/fc" -j4
"$work/fc/consumer"

echo "== find_package consumer"
cmake -S "$src" -B "$work/lib" -DMS110D_BUILD_TESTS=OFF "$@"
cmake --build "$work/lib" -j4
cmake --install "$work/lib" --prefix "$work/prefix"
cmake -S "$src/tests/consumer/findpackage" -B "$work/fp" "-DCMAKE_PREFIX_PATH=$work/prefix" "-DEXPECTED_VERSION=$version"
cmake --build "$work/fp" -j4
"$work/fp/consumer"
