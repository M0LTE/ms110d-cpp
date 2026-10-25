# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](docs/versioning.md).

## [Unreleased]

Nothing is released yet. What is on `main` so far:

### Changed

- Behaviour change: the transmitter sends the MIL-STD-188-110D standard mini-probe boundary pattern. The probe after data frame f (numbered from 0 over the burst) is cyclically shifted only when f mod L == L - 2, L being the frame count of the waveform's Long interleaver, whatever interleaver is selected. Before, the probe was shifted before the selected interleaver's boundary, as the C# 0.2.1 transmitter still does; the two are the same for the Long interleaver, so only ultra-short, short and medium bursts change (issue 68).

- Behaviour change: the receiver expects the standard mini-probe boundary pattern only; the detection of the C# 0.2.1 pattern (shifted at the selected interleaver's boundary) is removed. A current-pattern C# 0.2.1 transmission on the ultra-short, short or medium interleaver therefore decodes worse than with the C# receiver; the Long interleaver is unaffected (issue 68).

### Added

- An installable CMake library for PC and Cortex-M7, in single or double precision, with a C interface for other languages and .NET interop tests.
- Transmit: payload bits in, 9600 Hz audio out, in C++ and C, matching the C# reference audio.
- Receive building blocks: soft-decision decoders, a decision-feedback equaliser, preamble search and waveform identification, WN0 receive, and the equaliser receive path for WN1 to 8 and 13 with the turbo stage, the 8PSK salvage and ensemble and the MFB block decoder, expecting the standard mini-probe pattern (bit for bit with the C# on every reference case and on a corpus of fading, offset and drifting channels).
- Receive: the public receiver in C++ (`Receiver`) and C (`ms110d_rx_*`), audio in, events out (burst started, block decoded, burst completed, optional soft values), with the C#'s options and one shared receive front end; the same bits as the C# receiver on every reference case.
- User documentation: a README with install and quick starts in C++ and C, guides to the waveforms, transmitting, receiving and microcontroller builds, with every snippet compiled and run by the tests.
- Tests that compare the bits on the wire and the audio with the C# reference.
- The standard's D-LXIV performance masks (AWGN and Poor channel, WN0 to 8 and 13) and the D-LXV static channel for WN2, run by the C++ tests alone on channel audio made by the C# reference, with a long mode for the full counts.

[Unreleased]: https://github.com/M0LTE/ms110d-cpp/commits/main
