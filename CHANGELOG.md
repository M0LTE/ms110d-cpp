# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](docs/versioning.md).

## [Unreleased]

Nothing is released yet. What is on `main` so far:

### Added

- CMake project skeleton, test framework, public API lock and CI on x86-64 and Arm with both precisions, plus sanitizer and Cortex-M7 builds.
- A C interface with a .NET interop test project, and a pin to the C# reference release the reference files are generated from.
- Waveform tables and modes, scramblers, interleaver, framing, preamble and the whole-burst symbol stream.
- Tail-biting FEC encoder with the Table D-L puncturing.
- Modulator: pulse shaping and up-conversion to 9600 Hz audio on the 1800 Hz sub-carrier.
- Public transmit path from payload bits to audio, in C++ and C, checked against the C# reference audio.
- Receive building blocks: tail-biting Viterbi and SISO soft decoders, a probe-trained decision-feedback equaliser, preamble search and waveform identification, and WN0 Walsh receive.
- A framework for named, numbered deviations from the C# reference.
- Layer 1 tests that check the bits on the wire, side by side with the C# and stand-alone.

### Changed

- Every source file now carries a copyright header, and PROVENANCE records the dependency check.
