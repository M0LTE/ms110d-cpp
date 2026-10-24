# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](docs/versioning.md).

## [Unreleased]

Nothing is released yet. What is on `main` so far:

### Added

- An installable CMake library for PC and Cortex-M7, in single or double precision, with a C interface for other languages and .NET interop tests.
- Transmit: payload bits in, 9600 Hz audio out, in C++ and C, matching the C# reference audio.
- Receive building blocks: soft-decision decoders, a decision-feedback equaliser, preamble search and waveform identification, WN0 receive, and the equaliser receive path for WN1 to 8 and 13 with the turbo stage, the 8PSK salvage and ensemble and the MFB block decoder, accepting both mini-probe patterns (bit for bit with the C# on every reference case and on a corpus of fading, offset and drifting channels).
- Tests that compare the bits on the wire and the audio with the C# reference.

[Unreleased]: https://github.com/M0LTE/ms110d-cpp/commits/main
