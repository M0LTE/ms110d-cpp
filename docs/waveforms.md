# Waveforms

The 3 kHz serial-tone waveforms of MIL-STD-188-110D Appendix D, as this library does them. Adapted from the C# package's [waveforms page](https://github.com/M0LTE/M0LTE.Ms110d/blob/main/docs/waveforms.md) (same copyright holder), which has the page-by-page references into the standard.

"WN" is our short name for the standard's waveform number (the tables call it "Waveform Number", the text "WID"). Search the standard for WID 3, not WN3.

## What the library supports

- WN0 to WN8 and WN13, at 3 kHz only. Not WN9 to WN12 (32QAM to 256QAM), and none of the wider bandwidths.
- K7 and K9 convolutional codes, and all four interleavers: ultra-short, short, medium, long. WN0 has no ultra-short row in the standard, so asking for it is `Unsupported`.
- Transmit settings are free to choose any of these. The receiver follows whatever the preamble announces, up to the longest interleaver you set with `max_interleaver` (see [receiving](receiving.md)).

## Every waveform

| | |
|---|---|
| Symbol rate | 2400 symbols/s |
| Sample rate | 9600 Hz |
| Sub-carrier | 1800 Hz |
| Pulse shape | root raised cosine, roll-off 0.35 by default |
| Frame | a block of data symbols, then a mini-probe of known symbols (none on WN0) |
| Preamble | optional TLC section, then M super-frames of 240 ms each; it carries the waveform number, interleaver and constraint length |
| Code | rate 1/2 mother code, K7 or K9, punctured or repeated to the waveform's rate |
| Default transmit | short interleaver, K7, 3 super-frames, no TLC |

## Per waveform

| WN | Rate | Modulation | Code rate | Interleaver delay: ultra-short / short / medium / long | Target SNR: AWGN / Poor |
|---|---|---|---|---|---|
| 0 | 75 bps | Walsh | 1/2 | - / 533 ms / 1.92 s / 7.68 s | -6 / -1 dB |
| 1 | 150 bps | BPSK | 1/8 | 160 ms / 640 ms / 2.56 s / 10.24 s | -3 / 3 dB |
| 2 | 300 bps | BPSK | 1/4 | 160 ms / 640 ms / 2.56 s / 10.24 s | 0 / 5 dB |
| 3 | 600 bps | BPSK | 1/3 | 107 ms / 427 ms / 1.71 s / 6.83 s | 3 / 7 dB |
| 4 | 1200 bps | BPSK | 2/3 | 107 ms / 427 ms / 1.71 s / 6.83 s | 5 / 10 dB |
| 5 | 1600 bps | BPSK | 3/4 | 120 ms / 480 ms / 1.92 s / 7.68 s | 6 / 11 dB |
| 6 | 3200 bps | QPSK | 3/4 | 120 ms / 480 ms / 1.92 s / 7.68 s | 9 / 14 dB |
| 7 | 4800 bps | 8PSK | 3/4 | 120 ms / 480 ms / 1.92 s / 7.68 s | 13 / 19 dB |
| 8 | 6400 bps | 16QAM | 3/4 | 120 ms / 480 ms / 1.92 s / 7.68 s | 16 / 23 dB |
| 13 | 2400 bps | QPSK | 9/16 | 120 ms / 480 ms / 1.92 s / 7.68 s | 6 / 11 dB |

- **Interleaver delay** is how long one interleaver block lasts on air. The receiver decodes a block once it has all of it, so this is the delay before the first decoded bits, and each block is delayed the same.
- **Target SNR** is what the standard asks for, in 3 kHz, for a coded bit error rate of 1 in 100,000: on a steady channel (AWGN) and on the "Poor" channel (two equal fading paths, 2 ms apart, 1 Hz fading). It is the requirement, not a measurement of this library. Receive performance is held to within 0.5 dB of the C# receiver.
- **Rate** is the user data rate. Higher rates need a cleaner channel. Longer interleavers help on fading ones but cost delay and, in the receiver, a great deal of memory.

The same table with frame shapes and mini-probe details is in the C# package's page.
