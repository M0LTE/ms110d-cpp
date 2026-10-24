# Preamble detector

`src/preamble_detector.{hpp,cpp}` (`PreambleDetector`) finds a burst in a stream of 9600 Hz audio and identifies it from the preamble. It is internal for now (not installed, not in the locked API); the demodulator and the poll-and-drain C interface are built on it. Story: issue 24.

## The boundary in the C#

Ported from `Ms110dDemodulator` in M0LTE.Ms110d 0.2.0, from the audio input to the instant the receiver leaves `ReadingPreamble` for `Tracking`:

- the front end in `Process`/`PushT2`: mix down by 1800 Hz, the root-raised-cosine receive filter, decimate by two to the 4800 Hz "T/2" stream, the ring and the running comb energies;
- `SearchStep`, `Metric`, `AcceptPeak`: the matched filter over the 9 Walsh symbol Fixed subsection on a 7 bin grid (-75 to +75 Hz), peak picking against the 0.32 threshold, earliest-arrival walk-back, parabolic sub-sample timing, fine frequency from the phase progression of the nine segments;
- `TryReadPreamble`: downcount read and parity, the soft vote over up to five super-frames for the waveform ID (with the 0.20 margin gate) and the downcount (0.10 margin gate), the ID checksum and the defined-pair check, and `RefineCarrier` over the matched super-frame (`EstimateCarrierFit`).

Not ported: everything after the lock, including the second carrier fit over the last preamble super-frames, the input level estimate and the equaliser set up in `InitializeDfe`, tracking, and the presence/probe code (the pending C# change about probe patterns is after the lock, so it does not touch this). The C# also does not acquire a one-super-frame preamble (M = 1); neither does this.

## What it reports

`PreambleDetection`: waveform number, interleaver, constraint length, the downcount of the matched super-frame (`superframes_after`), the position of chip 0 and of the first data chip in T/2 samples (fractional), the same data start as an input sample position (the receive filter delay taken off, so it is where the first data symbol's pulse is centred in the received audio, the transmitter's `4 k + (taps - 1) / 2`), the sub-sample timing term, the carrier offset in Hz (fine fit included) and the carrier model the C# continues with (`carrier_omega`, `carrier_theta_base`), the peak metric and the number of input samples used at that point. `PreambleSearchStats` counts accepted peaks and each kind of rejection, for false alarm measurement.

## Using it, and the poll-and-drain interface

`process(audio)` takes any chunk size and returns how many samples it used. It stops right after the sample that completes a detection, so the caller can `take_detection()` before feeding the rest; with a detection waiting it uses nothing. After the event the detector is idle (the front end keeps running and `t2_sample()` serves the T/2 stream, so a demodulator can read the data from the same ring) until `resume_search()`, the C# `BackToSearch`. This is the shape the C interface wants (`docs/dev/c-interface.md`): `process` reports samples consumed so a full queue never loses input, the event carries its own sample position (`detected_at_sample`), and the queue is one event deep because the detector halts at the event. The event is plain numbers and enums of fixed values, ready to map to a `uint32_t kind` event struct. The ring holds 8192 T/2 samples (1.7 s), enough for the longest wait (five super-frames); the demodulator needs 65536 for the long interleaver and will own a bigger ring or take the samples as they arrive.

Nothing allocates after construction (the object is about 90 KB, so it does not belong on a small stack) and nothing throws. Cost is dominated by the search: 7 bins by 288 complex multiply-accumulates per T/2 sample, about 10 million per second of audio, which the Cortex-M7 port will have to measure.

## Precision

The detector is the same in both precision builds, because the C# fixes each choice and the task is to match it. Samples, the filter, the mixer table, the ring and the correlators are single precision (the C# `Complex32`, written out in `src/dsp/complex_f.hpp` so products and sums round the same way). Energies, the normalised metric, the timing and frequency maths, the interpolator weights and the vote sums are double. Trigonometry is `float` for phasors and `double` for the interpolator, as in the C#. The filter taps are the transmitter's `design_pulse` (designed in double, stored as float). `-ffp-contract=off` keeps compiler fusing out. On the Cortex-M7 the double parts use the double precision FPU.

The receive filter is computed only for the samples that are kept (every second one); the history is still pushed for all, so the result equals the C# float to the bit. The filter is `src/dsp/fir_filter.hpp`, the port of M0LTE.Dsp `FirFilter`.

## Tests

`tests/preamble_detector_test.cpp`, against `tests/data/csharp-detect-0.2.0.txt`, which the C# tool writes (`dotnet run -c Release --project tools/csharp-vectors -- detect build/_reference/ref > tests/data/csharp-detect-0.2.0.txt`; regenerate when the pin moves). The tool feeds the real demodulator one sample at a time, stops at the lock and reads the private carrier and timing fields by reflection.

- every one of the 122 reference cases: decision, parameters, downcount, data start, carrier state and the sample it completed on equal the C# (and the transmitted parameters; the data start is within half a sample of the true one);
- 406 impaired scenarios: the reference baseband delayed by a fractional number of samples, put on a carrier 1800 Hz plus a chosen offset (sweep -90 to +90 Hz), with lead-in and seeded noise at stated SNRs (-9 to +12 dB in 3 kHz), plus a lower threshold run that exercises the rejection gates;
- 30 noise-only streams of 12 s at three thresholds: accepted peaks, rejections and false detections equal the C#, and the test requires them to be no more than the C#'s;
- chunk size independence, state handling, transmitter audio of every waveform without the reference files, and zero allocation.

The impairment and the noise generator (splitmix64, twelve uniforms summed for a unit variance value) are specified by the code in `tools/csharp-vectors/DetectDump.cs` and `tests/support/impair.cpp`, which must stay identical; the dump holds every parameter of every line. Agreement is bit-identical in practice: the tolerances in the test are 1e-6 on positions and frequency and 1e-9 on the phase increment, and the measured worst differences are 0.
