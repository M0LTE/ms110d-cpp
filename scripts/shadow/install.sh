#!/bin/sh
# Copyright (C) 2026 Thomas Fanning
# SPDX-License-Identifier: AGPL-3.0-or-later
# Installs the shadow decode pipeline (docs/dev/shadow.md) on the recording container. Run as root, from a
# checkout of this repository, with the C# twin already published for the container:
#   dotnet publish tools/csharp-vectors -c Release -r linux-x64 --self-contained -p:InvariantGlobalization=true -o /tmp/cs
#   scripts/shadow/install.sh /tmp/cs
# SHADOW_GIT labels the build when the source is not a git checkout. Needs cmake, g++ and python3 (apt install cmake g++ python3). Safe to rerun: it rebuilds and replaces the files.
set -eu
cs_dir=${1:?usage: install.sh DIR_WITH_PUBLISHED_CSHARP_TWIN}
src=$(cd "$(dirname "$0")/../.." && pwd)
home=${SHADOW_HOME:-/opt/ms110d-shadow}

mkdir -p "$home/bin" "$home/cs"
cmake -S "$src" -B "$home/build" -DCMAKE_BUILD_TYPE=Release -DMS110D_BUILD_TOOLS=ON \
  -DMS110D_TOOL_GIT="${SHADOW_GIT:-}" -DMS110D_BUILD_TESTS=OFF -DMS110D_INSTALL=OFF -DMS110D_BUILD_C_SHARED=OFF >/dev/null
cmake --build "$home/build" -j"$(nproc)" --target ms110d-decode
install -m 755 "$home/build/tools/ms110d-decode/ms110d-decode" "$home/bin/ms110d-decode"
rm -rf "$home/cs.new" && cp -a "$cs_dir" "$home/cs.new"
rm -rf "$home/cs" && mv "$home/cs.new" "$home/cs"
install -m 755 "$src/scripts/shadow/compare.py" "$home/compare.py"
install -m 755 "$src/scripts/shadow/ms110d-shadow-decode" /usr/local/sbin/ms110d-shadow-decode
for u in ms110d-shadow-decode.service ms110d-shadow-decode.timer ms110d-shadow-report.service ms110d-shadow-report.timer; do
  install -m 644 "$src/scripts/shadow/systemd/$u" /etc/systemd/system/$u
done
systemctl daemon-reload
systemctl enable --now ms110d-shadow-decode.timer ms110d-shadow-report.timer
echo "installed; try: ms110d-shadow-decode all"
