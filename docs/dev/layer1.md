# Layer 1: bits on the wire

Layer 1 of [docs/plan.md](../plan.md) checks that the C++ and the C# reference produce identical bits at every stage of the transmit pipeline, and names the first stage that differs. It has two halves.

## Side by side, in process

`tests/interop/Layer1Tests.cs` runs seeded random frames through the C# (public API of M0LTE.Ms110d only) and the C++ and compares the outputs exactly. There is one test per combination of waveform number, interleaver length and constraint length (78 of them: WN 0 has no ultra-short interleaver, K7 and K9 for each). A frame is a random payload of 1 to two interleaver blocks of bits, with random EOM and preamble settings, taken through every stage; the block stages run once per block. Each stage is fed the C# output of the stage before, so the stage that differs is named even though everything upstream agreed.

The C# types driven: `Ms110dFraming.BuildTxBits` (payload, EOM and zero fill), `PreambleGenerator`, `TailBitingEncoder` (M0LTE.Fec), `Ms110dPuncture`, `Ms110dInterleaver`, `Ms110dFraming.EncodeBlock` (the composed encode, puncture and interleave, compared with the three C++ stages chained), `Ms110dScrambler` and `Trinomial159Scrambler`, with `Ms110dMode` and `Ms110dInterleaverParams` for the geometry. Transcoding and the whole-burst symbol stream are not public in the C# (`Ms110dModulator.BuildSymbols` is internal and the public `Modulate` returns audio, which is layer 2), so they have no side-by-side check; the burst symbols are checked against the reference files below, and the C++ transcode output feeds both scramblers, transcoding against the C# values in `reference_tables_test.cpp`.

The C++ stages are internal, so they are reached through `ms110d_testhooks`, a shared library built only with the tests (`tests/testhooks`). It is never installed, is not in the public header, the API lock or the `ms110d_c` exports, and follows the conventions of [c-interface.md](c-interface.md). ctest passes its path in `MS110D_TESTHOOKS_LIB`.

### Counts

| Run | Frames per combination | How |
| --- | --- | --- |
| Default (every ctest and CI run) | 300 | nothing to set |
| Full, the plan's gate | 100,000 | `MS110D_L1_FULL=1` |
| Anything else | n | `MS110D_L1_FRAMES=n` |

`MS110D_L1_WN=1,2` restricts to some waveform numbers (used to shard the full run in CI), `MS110D_L1_THREADS` sets the thread count (default: the core count, at most 4). Frame i of a combination has a seed derived from the combination and i alone, so a failure prints a seed and a frame number that reproduce it whatever the thread count. Full run by hand:

    cmake -S . -B build -DMS110D_DOTNET_TIMEOUT=20000
    MS110D_L1_FULL=1 ctest --test-dir build -R dotnet_interop --output-on-failure

The scheduled and manually dispatched workflow `.github/workflows/layer1-full.yml` runs the full count, one job per waveform number.

## Stand-alone, against the reference files

`tests/layer1_reference_test.cpp` (test name starts with `reference `, so the CI reference job runs it) recomputes, for each of the 122 cases, framing, the preamble, encoding, puncturing, interleaving, scrambling and the whole burst symbol stream with the mini-probe values, each from the previous stage's reference file, and reports the first stage that differs. `tests/framing_test.cpp` has further checks of the same files (block-at-a-time assembly, the waveform ID and downcount fields).

## Adding a stage

Both tests keep their stages in one ordered list (`CheckFrame` in `Layer1Tests.cs`, `first_differing_stage` in `layer1_reference_test.cpp`). A new stage is a hook in `tests/testhooks`, a `Stage_` method and a call in the list, and a block in the C++ function. When the C# makes the burst symbols public, add a burst stage after the block loop.

## Proving the tests can fail

`scripts/mutation-check-layer1.sh` breaks one stage at a time in a copy of the tree (the EOM word, a preamble section, a convolutional polynomial, a puncture mask, the interleaver, the PSK scrambler, the Trinomial shift count, the probe shift rule) and checks that both tests name that stage; the probe rule has no public C# entry point, so only the stand-alone test is asked. The working tree is not touched.
