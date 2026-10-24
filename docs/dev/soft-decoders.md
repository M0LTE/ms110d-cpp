# Soft decoders

Four ports from the C# package, all internal (headers in `src/`, not installed, not in the locked API):

| C++ | C# | Where |
| --- | --- | --- |
| `fec::TailBitingViterbiDecoder` | `M0LTE.Fec.TailBitingViterbiDecoder` | `src/fec/tail_biting_viterbi.{hpp,cpp}` |
| `fec::TailBitingSisoDecoder` | `M0LTE.Ms110d.Fec.TailBitingSisoDecoder` | `src/fec/tail_biting_siso.{hpp,cpp}` |
| `ms110d::Bcjr` | `Ms110dBcjr` (max-log, shift register trellis) | `src/bcjr.{hpp,cpp}` |
| `ms110d::ChainBcjr` | `Ms110dChainBcjr` (exact, one chain per echo residue class) | `src/bcjr.{hpp,cpp}` |

The two FEC decoders are in `m0lte::fec` and know nothing of MS110D. The BCJRs are in `m0lte::ms110d`, in the precision keyed inline namespace.

## Shape

- Each decoder is an object sized in its constructor from the longest block (`max_block_bits`, `max_symbols`, `max_delay`, `max_constellation`). `decode()` and `equalize()` never allocate and never throw; a bad argument (wrong length, block shorter than k, longer than the maximum, delay out of range) returns false and writes nothing, where the C# throws. The tests run each decode inside an `AllocationScope` and require zero allocations.
- The constructor does allocate (vectors). A bare metal build that must not use the heap at all would want a variant that takes caller storage; nothing needs it yet.
- Memory to know about: the SISO keeps the forward metrics for the whole block, `max_block_bits * 2^(k-1)` floats (0.5 MB for k = 9 and 1000 bits), as the C# does. The shift register `Bcjr` keeps two `(n + 1) * 2^delay` tables. The chain BCJR needs `n * M` reals. Check these against the M7's RAM when the receiver is sized.
- Real MS110D sizes (the longest blocks): the SISO forward store is 50 MB at K9 and 12.6 MB at K7 for WN8 long (49152 info bits), and 12.6 MB at K9 for WN8 medium. The Viterbi decision memory is 1.58 MB at K9 and 394 KB at K7. The chain BCJR needs 17.6 KB per frame. A microcontroller build will need windowed decoding for the SISO; that is tracked as M0LTE/ms110d-cpp#44.
- Depuncturing is not in these classes in the C#: it is `Ms110dPuncture.Depuncture` and the soft apply next to it. They belong with the puncture module and come with the receive chain; the decoders take the full mother lattice with 0 for erased positions, as the C# ones do. The tests cover erased positions by zeroing LLRs.
- Viterbi and SISO use a wrap of `min(6 * k, N)` stages each end and equal start metrics, exactly as the C#. Bit order, tie breaking (`m1 > m0`, first best state) and the renormalisation every 1024 steps are copied so the output is identical, not merely equivalent.

## Where float and where `real`

- **Viterbi and SISO: always float**, in both precision builds, like the C#. The module is independent of the modem's `real`. The metrics are sums of a handful of LLRs, and the SISO's max* uses a 128 entry correction table with a worst case error of 0.04, which dwarfs float rounding, so double would improve nothing while doubling the SISO's alpha store. The interface is float spans: the caller scales its LLRs.
- **Max-log BCJR and chain BCJR: `real`.** The C# is float, but these run on channel estimates and samples that will be `real` (the demodulator's equaliser output), and their metrics are squared distances divided by a noise variance, which spread over a wide range. In the single build this is the C# arithmetic exactly; in the default double build it is more precise than the C# and the LLRs differ only by the rounding the C# carries. The constellation and the preceding symbols are float tables (`tables::Complex32`, widened on use), as in the C#. Complex products are written out on real and imaginary parts, in the C# order, so no library NaN recovery runs.
- The sentinel for an impossible path is the C# `-1e30f`, and the SISO's initial norm is `FLT_LOWEST` (the C# `float.MinValue`).

## Tests and tolerances

`tests/soft_test.cpp` against `tests/data/csharp-soft-0.2.0.txt`, made by `dotnet run -c Release --project tools/csharp-vectors -- soft > tests/data/csharp-soft-0.2.0.txt`. The tool calls the public C# decoders; `Ms110dBcjr` and `Ms110dChainBcjr` are internal and take spans, so it reaches them through a `DynamicMethod` that skips visibility checks and converts arrays to spans.

Inputs are not stored. Both sides regenerate them from an integer xorshift32 generator (Gaussian noise as the sum of twelve 16 bit draws, exact in double, then only `+ - * /` on doubles), and each line carries an FNV-1a checksum of every input float's bits, so a generator that drifts fails as itself. The dump holds the outputs: decoded bits, SISO posteriors and BCJR LLRs as IEEE bits.

| What | Compared | Bound | Measured |
| --- | --- | --- | --- |
| Viterbi decoded bits | exact | equal | equal, 26 blocks (K7 and K9; n = 40, 100, 360; noisy, erased, saturated and all zero input) |
| SISO posteriors | absolute difference | 1e-3 | 0 in both precisions |
| Bit error counts and checksums of every decoded bit (Viterbi) and every coded decision (SISO sign) over 14 curve points, 1000 blocks of 120 bits each | exact | equal | equal |
| Max-log BCJR | `abs(a - b) / (1 + abs(b))` | 1e-6 single, 1e-4 double | 0 single, 1.2e-5 double |
| Chain BCJR (BPSK, QPSK, 8PSK; priors, per symbol noise, no preceding, delay beyond the block) | same | 1e-6 single, 1e-4 double | 0 single, 2.8e-5 double |

Why these bounds. The Viterbi has no libm in it, so it is exact. The SISO uses libm only to build the 128 entry correction table; on the same glibc that is bit identical (measured difference 0). A libm that rounded one entry one ulp apart could, very rarely, move a difference across a 1/16 table step and change one metric by up to 0.04; 1e-3 is the bound for the common case and a failure there should be looked at, not silently loosened. The single build BCJRs use the same float operations in the same order as the C#, so zero is expected and 1e-6 allows only libm `log` and `exp` ulps (6e-8 relative). The double build computes in double what the C# computes in float; the float rounding error of the C# metrics sets the difference (about 1e-5 relative), and 1e-4 leaves a few times that.

The behaviour test (`bit error rate curves match the C# exactly`) feeds the same seeded noisy encoded blocks through the C++ encoder and both decoders at Eb/N0 from 0 to 4.4 dB (rate 1/2 BPSK, Eb/N0 = 1 / sigma^2) and prints the curve. It requires the C++ error counts to equal the C# counts at every point, which they do, along with the checksums of all decisions, and that the Viterbi error rate falls as the noise falls. Because the arithmetic is identical, the "within noise" bar is met with zero difference.

| Eb/N0 | K7 info BER (Viterbi) | K9 info BER (Viterbi) |
| --- | --- | --- |
| 0.00 dB | 1.6e-1 | 1.8e-1 |
| 0.92 dB | 4.4e-2 | 4.1e-2 |
| 1.94 dB | 6.3e-3 | 2.9e-3 |
| 2.50 dB | 1.2e-3 | 7.1e-4 |
| 3.10 dB | 4.4e-4 | 0 |
| 3.74 dB | 9.2e-5 | 0 |
| 4.44 dB | 0 | 0 |

(120 bit tail-biting blocks, 120000 bits per point, so a floor of about 1e-5.)
