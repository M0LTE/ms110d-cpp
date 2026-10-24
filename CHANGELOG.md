# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](docs/versioning.md).

## [Unreleased]

Nothing is released yet. What is on `main` so far:

### Added

- An installable CMake library for PC and Cortex-M7, in single or double precision, with a C interface for other languages and .NET interop tests.
- Transmit: payload bits in, 9600 Hz audio out, in C++ and C, matching the C# reference audio.
- Receive building blocks: soft-decision decoders, a decision-feedback equaliser, preamble search and waveform identification, and WN0 receive.
- Tests that compare the bits on the wire and the audio with the C# reference.

[Unreleased]: https://github.com/M0LTE/ms110d-cpp/commits/main
