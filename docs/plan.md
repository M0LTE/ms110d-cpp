# MS110D in C++: plan

Agreed 2026-10-24. This is a living document; changes are recorded in the amendment log at the end.

## Summary

We build a second MS110D modem in C++ that works on air with the existing C# one, and prove it with tests rather than claims. It mirrors the C# package split, so the waveform is a library on its own with no ties to pdn-soundmodem.

It serves three uses from one code base: commercial licensing for other products, a microcontroller board in the NinoTNC's dsPIC socket, and a host library. The C# stays the reference; where the port shows the C# is wrong, we fix the C# too, so both end up right.

The C# package is about 9,800 lines and has only ever talked to itself. A second implementation is also our first real interoperability check of the C#.

## Goal: compatible on air, in practice

Each implementation decodes the other's transmissions as well as it decodes its own. Identical internal numbers are not the goal; identical behaviour on air is.

Concretely, the two must agree on:

- **The bits on the wire, exactly.** For any frame, mode, interleaver and code, both produce the same symbol stream: preamble, WID, scrambling, FEC, puncturing, interleaving, mini-probes, EOM and EOT.
- **The transmitted audio, closely.** The same symbols at 9600 Hz on the 1800 Hz carrier, with the same pulse shape and level, to within a small numerical tolerance.
- **Receive performance, in practice.** On the same signals, each decodes about the same share of frames, within 0.5 dB across the SNR range, on simulated channels and on real off-air audio.
- **The public shape.** Same modes, settings and events, so a caller can swap one for the other.

## Licensing

Public under AGPL-3.0-or-later, the same as the C#, with separate commercial licences offered by the copyright holder.

- Every commit in M0LTE.Ms110d is the copyright holder's, and its PROVENANCE.md records that no third-party source was consulted: constants come from the published standard, the DSP from textbooks.
- To confirm before the first commercial deal: M0LTE.Dsp, M0LTE.Fec and M0LTE.Ofdm carry the same clean provenance.
- Outside contributions only with a contributor licence agreement, or not at all, so the commercial route stays open.
- This repo has its own [PROVENANCE.md](../PROVENANCE.md) saying it is a port of the C# reference plus the standard, and nothing else.

## Package structure

The C++ waveform is a library on its own, mirroring M0LTE.Ms110d: bits in, 9600 Hz audio out, and back. It knows nothing of KISS, sound cards or pdn-soundmodem.

| C# today | C++ counterpart |
| --- | --- |
| M0LTE.Ms110d (waveform) | This repo, one CMake library: namespace `m0lte::ms110d`, headers under `m0lte/ms110d/`, CMake target `m0lte::ms110d`. Same type names: modulator, demodulator, tx settings, modes, tables, framing. |
| M0LTE.Dsp, M0LTE.Fec | Only the pieces MS110D uses, as separate modules inside this repo, laid out so they can move to their own libraries when a second C++ modem needs them. |
| M0LTE.Ofdm (only for its complex type) | `std::complex`; no counterpart needed. |
| M0LTE.HfChannel (tests only) | Not ported at first. The C# channel makes the impaired audio and both receivers decode the same files. |
| IModem wrapper in pdn-soundmodem | Stays out, as in C#. The NinoTNC firmware and any commercial product write their own thin wrapper. |

Design rules so the same code builds on a PC and a Cortex-M7:

- C++20, no exceptions or RTTI in the core, no memory allocation after start-up; buffers sized up front from the mode and interleaver.
- Precision chosen at build time (double or single), double by default.
- A small C interface on top, so C#, Python or anything else can load it. The C# tests use this to drive both implementations side by side.
- A public API lock test, and semantic versioning, as in the C# package.
- Distributed as plain CMake source that installs properly, so users can take it with `find_package`, `FetchContent` or a git submodule. A vcpkg port at 1.0 if anyone asks; no Conan unless asked.

## How we prove compatibility

Four layers of tests, each run on every change, covering every mode (WN0 to WN8, WN13), both interleavers and both codes.

| Layer | What is compared | Pass means |
| --- | --- | --- |
| 1. Bits on the wire | Random frames through each stage of the bit pipeline, in both: framing, scrambler, encoder, puncture, interleaver, WID, probes, EOM | Identical, bit for bit |
| 2. Transmit audio | The same frames modulated by both | Same length, samples within a tight tolerance, same spectrum and level |
| 3. Cross-decoding | C# to C#, C# to C++, C++ to C#, C++ to C++, through the C# HF channel at a ladder of SNRs on AWGN, Good, Moderate and Poor | Each pairing's frame loss within 0.5 dB of the others |
| 4. Real audio | Off-air recordings of GB7RDG and of the mailcast receivers | The C++ decodes every frame the C# does, give or take the documented deviations |

Two ways of running them:

- **Side by side.** A .NET test project loads the C++ through its C interface and compares the two in-process on thousands of random seeds. A mismatch names the first stage that differs.
- **Stand-alone.** The C++ has its own tests against reference files made by the C#, plus the standard's own worked examples and performance masks (D-LXIV and D-LXV). So it proves itself without .NET, including on the microcontroller.

One trap to guard against: a port copies the original's misreadings of the standard. The standard's tables are not re-checked: they were transcribed twice and verified for the C#. But the logic of each stage (bit order, probe placement, scrambler stepping and so on) is read against the standard as it is ported, and any disagreement is raised, not copied.

## Live side by side on a mailcast receiver

A shadow receiver decodes GB7RDG's real hourly bulletins with both modems on the very same audio, and reports where they differ.

- **Leave the live receiver alone.** The shadow runs on its own host, taking its own feed from the same SDR as an existing mailcast receiver.
- **Same audio, both decoders.** One audio stream is split to the C# and C++ demodulators, so any difference is the modem, not the radio path.
- **Keep the evidence.** Each slot's audio is kept, so every disagreement can be replayed, fixed, and added to the layer 4 tests.
- **A daily report.** Bursts and frames decoded by both, by C# only and by C++ only, with SNR, plus processor time per slot for each.

Pass for this stage: a fortnight of slots with no unexplained differences. Later, the same shadow can run the single-precision build to measure what it costs on real signals.

## Deviations and changing the C#

The standard is the yardstick, not either implementation. When the two disagree, we find out which matches MIL-STD-188-110D and fix the other.

1. **Off-air bug in the C#** (it departs from the standard in a way another station would notice): fix the C#, regenerate its reference files, and release it. The maintainer approves each one first, because GB7RDG and the mailcast receivers all need a co-ordinated upgrade.
2. **Internal bug in the C#** (wrong, but nothing on air changes): fix the C# and move on.
3. **Accepted deviation** (a difference we keep on purpose, for example the microcontroller build skipping the turbo pass): written as a named test that measures it and states why, in a deviations list in this repo. If the cost grows, the test fails.

No deviation exists only in prose. Each one is a test with an issue number.

## Microcontroller target

An STM32H7 Nucleo, wired into the NinoTNC N9600A's dsPIC socket through an adapter, running the same C++ library as the PC build. Only if we decide to run it on hardware ourselves; the adapter design and the NinoTNC schematic are deferred until then.

- **Why the H7.** It has hardware double precision, so it can run the default build unchanged and be held to the same tests. The current dsPIC has 48 KB of RAM and no floating point, far too small.
- **What already fits.** The NinoTNC sends transmit audio as one filtered PWM pin (RB10, then an 8160 Hz Bessel filter), which a fast timer on the H7 can drive. Its USB serial runs at 57600 baud, plenty for MS110D. With the short interleaver the receiver needs about 60 KB of sample buffer.
- **What is unknown.** Processor load. On a PC the BPSK modes take 6 to 10% of one Ryzen core with turbo decoding; a Cortex-M7 is many times slower, so this could be tight. The first job on the Nucleo is to measure it.
- **If it is too slow.** Single precision, a lighter equaliser or skipping the turbo pass, in that order. Each is an accepted deviation with its test, and the shadow receiver measures its cost on real signals.
- **Scope first.** The BPSK modes (WN1 to WN5) and the short interleaver; denser modes later if the load allows.
- **Power.** The NinoTNC's 250 mA regulator will likely need replacing.

## Stages

Each stage ends with a merged, reviewed PR and its tests passing. Stages 6 and 7 can run side by side once receive works. Each stage is an epic in the issues.

1. **Ground work.** CI building on Linux x64 and arm64 plus a Cortex-M7 compile check. A tool in the C# repo that writes reference files for every stage. Provenance check of M0LTE.Dsp, Fec and Ofdm. Gate: CI green on an empty library.
2. **Bits on the wire.** The whole bit pipeline for every mode, both interleavers, both codes. Gate: layer 1 identical on 100,000 random frames per combination.
3. **Transmit.** Preamble, modulator, EOM and EOT. Gate: layer 2 within tolerance, and the C# demodulator decodes everything the C++ sends.
4. **Receive, BPSK and Walsh first.** WN0 to WN5, short interleaver, with turbo decoding. Gate: layers 3 and 4 pass for those modes, and the standard's performance masks pass.
5. **Receive, the rest.** WN6 to WN8 and WN13, the long interleaver, the MFB decoder. Gate: layers 3 and 4 pass for every mode.
6. **Shadow receiver.** Both decoders on GB7RDG's live bulletins. Gate: two weeks with no unexplained differences.
7. **Nucleo.** If we go to hardware: buy a NUCLEO-H743ZI2 and measure the load first, then PWM transmit, audio receive and KISS over the NinoTNC's USB serial. On-air receive test before any transmitting. Gate: frames exchanged with a pdn-soundmodem station on air.
8. **Release 1.0.** Version 1.0 of the library with the API locked; commercial licences available from this point.

## Decisions

- Repo: M0LTE/ms110d-cpp, public under AGPL from the first commit.
- Licensing: AGPL plus commercial is an established pattern; no further legal work for now.
- Hardware: NUCLEO-H743ZI2, bought only if we decide to run on hardware ourselves.
- Shadow receiver: its own host, separate from the live mailcast receiver.
- NinoTNC schematic and adapter: deferred.
- Distribution: installable CMake source; vcpkg at 1.0 on demand.

## Amendment log

- 2026-10-24: plan agreed.
- 2026-10-24: the standard's tables are not re-checked during the port, since they were transcribed twice and verified for the C#; the C++ tables are checked against the C# only (Tom).
