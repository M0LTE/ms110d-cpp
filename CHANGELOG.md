# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](docs/versioning.md).

## [Unreleased]

Nothing is released yet. What is on `main` so far:

### Changed

- The C# reference moves from M0LTE.Ms110d 0.3.0 to 0.4.1, and the receiver follows its receive fixes (issue 98; C# issues 7, 8, 10 and 17). Every waveform with mini-probes except WN 8 now scales the received signal to a fixed level before the equaliser starts, up or down (0.25 for WN 1 and 2, 0.5 for WN 3 to 7 and 13, at most 256 times), so decoding no longer depends on the receive level; WN 8 keeps the old boost of weak signals only. A preamble read that fails after the search stepped back to an earlier echo is read once more at the strongest peak. While a burst is received the search runs on in the background, and a new preamble that passes every check ends the burst with the new end reason `NewPreamble` (C `MS110D_END_NEW_PREAMBLE`, 5) and is received at once, so a burst that lost its EOM no longer deafens the receiver to the next transmission. The C# fix for a crash after a change of block size (its issue 9) does not apply: the C++ sizes its buffers for the longest block. The reference set is unchanged apart from its manifests (the transmitter did not change); the receive dumps are regenerated, and the C++ gives the C# 0.4.1 receiver's results on every exact comparison in both precisions, on the C#'s new acquisition scenarios (new `AcquisitionTests` interop tests) and on corpus-v2 (all 255 good frames over 79 bursts, the same end and block count on every burst, 40 of 40 error-free payloads equal). The receiver's memory grows by 128 bytes.

- Behaviour change: the transmitter sends the MIL-STD-188-110D standard mini-probe boundary pattern. The probe after data frame f (numbered from 0 over the burst) is cyclically shifted only when f mod L == L - 2, L being the frame count of the waveform's Long interleaver, whatever interleaver is selected. Before, the probe was shifted before the selected interleaver's boundary, as the C# up to 0.2.1 did; the two are the same for the Long interleaver, so only ultra-short, short and medium bursts change (issue 68).

- Behaviour change: the receiver expects the standard mini-probe boundary pattern only; the detection of the old C# pattern (shifted at the selected interleaver's boundary) is removed. A transmission from a C# 0.2.1 or earlier transmitter on the ultra-short, short or medium interleaver therefore decodes worse than with a C# 0.2.1 receiver; the Long interleaver is unaffected (issue 68).

- The C# reference moves from M0LTE.Ms110d 0.2.1 to 0.3.0 (issues 94 and 68), which sends and receives only the standard mini-probe pattern and removes its internal pattern switch. The reference set changes in 103 cases (the ultra-short, short and medium cases of WN1 to WN13; files 10, 12 and 13), the committed C# dumps are regenerated, and the test tooling no longer reaches into the C# by reflection to force the pattern. The exact C++ against C# comparisons all hold with nothing forced. On the 31 recorded GB7RDG bursts (current-pattern audio) the C++ receiver now matches the C# 0.3.0 receiver like for like: 97 of 97 good frames, the same end reason and block count on 31 of 31, error-free payloads equal on 5 of 5; the layer 4 rule is applied to every burst against the live C# and the issue 68 floor tests are gone.

### Added

- corpus-v2, the layer 4 real-audio corpus recorded after GB7RDG switched to the standard mini-probe pattern, replaces corpus-v1 (old-pattern audio; issue 68): 77 bursts (WN 3 and WN 4, Short interleaver, +1 to +18 dB, 30 error-free, 11 with no good frame, 2 across a dropout), 25.6 MB, expected results from the pinned C# 0.3.0. The committed set changes to two bursts from it (0.7 MB), the C# floors become 234 good frames (fetched corpus) and 7 (committed set), and the C++ receiver meets the layer 4 rule on all of it: 234 of the C#'s 234 good frames, the same end reason and block count on 77 of 77, error-free payloads equal on 30 of 30, no rule problem.

- An installable CMake library for PC and Cortex-M7, in single or double precision, with a C interface for other languages and .NET interop tests.
- Transmit: payload bits in, 9600 Hz audio out, in C++ and C, matching the C# reference audio (with the standard mini-probe pattern).
- Receive building blocks: soft-decision decoders, a decision-feedback equaliser, preamble search and waveform identification, WN0 receive, and the equaliser receive path for WN1 to 8 and 13 with the turbo stage, the 8PSK salvage and ensemble and the MFB block decoder, expecting the standard mini-probe pattern (bit for bit with the C# on every reference case and on a corpus of fading, offset and drifting channels).
- Receive: the public receiver in C++ (`Receiver`) and C (`ms110d_rx_*`), audio in, events out (burst started, block decoded, burst completed, optional soft values), with the C#'s options and one shared receive front end; the same bits as the C# receiver on every reference case (standard mini-probe pattern).
- User documentation: a README with install and quick starts in C++ and C, guides to the waveforms, transmitting, receiving and microcontroller builds, with every snippet compiled and run by the tests.
- Tests that compare the bits on the wire and the audio with the C# reference.
- A command line decoder, `ms110d-decode` (`-DMS110D_BUILD_TOOLS=ON`), and a shadow receiver that runs it beside the C# on recorded GB7RDG bulletins and writes a daily comparison.
- The standard's D-LXIV performance masks (AWGN and Poor channel, WN0 to 8 and 13) and the D-LXV static channel for WN2, run by the C++ tests alone on channel audio made by the C# reference, with a long mode for the full counts.

[Unreleased]: https://github.com/M0LTE/ms110d-cpp/commits/main
