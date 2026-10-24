#!/usr/bin/env python3
# Turns the text printed by `dotnet run ... -- fec` into tests/fec_vectors.inc.
# Usage: make_fec_inc.py fec.txt > tests/fec_vectors.inc
import sys

sec = None
enc, pun, full = [], [], []
for l in open(sys.argv[1]):
    l = l.strip()
    if l.startswith('// ENCODE'):
        sec = enc; continue
    if l.startswith('// PUNCTURE'):
        sec = pun; continue
    if l.startswith('// FULL'):
        sec = full; continue
    sec.append(l.split())
o = ['// Copyright (C) 2026 Thomas Fanning', '// SPDX-License-Identifier: AGPL-3.0-or-later', '',
     '// Generated from the C# reference by tools/csharp-vectors (fec mode) and make_fec_inc.py. Do not edit by hand.', '']
o.append('struct EncodeVec { int k; std::uint32_t seed; std::size_t n; std::uint64_t fnv; };')
o.append('constexpr EncodeVec encode_vectors[] = {')
for c, s, n, h in enc: o.append(f'    {{{c[1]}, {s}u, {n}, {h}ull}},')
o.append('};\n')
o.append('struct PunctureVec { int k; unsigned num; unsigned den; std::uint32_t seed; std::size_t n; std::size_t out_len; std::uint64_t fnv; };')
o.append('constexpr PunctureVec puncture_vectors[] = {')
for c, r, s, n, L, h in pun:
    a, b = r.split('/'); o.append(f'    {{{c[1]}, {a}, {b}, {s}u, {n}, {L}, {h}ull}},')
o.append('};\n')
o.append('struct FullVec { int k; unsigned num; unsigned den; std::uint32_t seed; std::size_t n; const char* info; const char* coded; const char* punctured; };')
o.append('constexpr FullVec full_vectors[] = {')
for c, r, s, n, i, cd, p in full:
    a, b = r.split('/'); o.append(f'    {{{c[1]}, {a}, {b}, {s}u, {n}, "{i}", "{cd}", "{p}"}},')
o.append('};')
print('\n'.join(o))
