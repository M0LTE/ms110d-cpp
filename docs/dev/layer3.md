# Layer 3: cross-decoding harness

Issue 28, under epic 23. Code: `tests/interop/Layer3/`. It sends seeded bursts from a transmitter through the C# HF channel (`M0LTE.HfChannel`), at a ladder of SNRs, to a receiver, and records frame and bit errors. Pass means the SNR at which each pairing reaches the target frame loss agrees within 0.5 dB.

## What is measured

- A frame is one burst carrying exactly one interleaver block (payload = block size - 32 bits, so the EOM ends the block), 3 preamble super-frames. It is lost if no burst is decoded or the payload differs in any bit. Bit errors are counted too (a missing decode counts every bit).
- Channels: `awgn`, and the ITU-R F.1487 Good (2 paths, 0.5 ms, 0.1 Hz), Moderate (1 ms, 0.5 Hz) and Poor (2 ms, 1 Hz, the D.6.1 channel). SNR is in 3 kHz.
- Target: 10% frame loss on AWGN, 20% on the fading channels. Fading curves are shallow (a few % per dB) and on a short block have a floor of a few %, so 10% is ill-conditioned there.
- Ladder: per cell, found automatically (the reference pairing on 6 frames, 2 dB steps from well below the AWGN mask), then 10 points at 0.5 dB from 2 dB below to 2.5 dB above. If a curve does not bracket the target the ladder moves 3 dB and the cell reruns (twice at most); an unbracketed curve fails.
- Threshold: curve made non-increasing (pool adjacent violators), then linear in log(loss) between the bracketing points. The unpaired 95% interval comes from Wilson bounds on every point.

## Same realisation for every pairing

The payload seed depends on (mode, frame) and the channel seed on (channel, mode, frame). Neither depends on the transmitter, receiver or SNR, so every pairing sees the same fading and noise and differences come from the modems. Every cell report prints how many frame decodes differ between pairings and a paired bootstrap 95% interval for the threshold difference (frames resampled together).

## Confidence, honestly

Frames per point: 40 on AWGN and 64 on Moderate in the default run; 60 on AWGN and 160 on the fading channels in long mode. On AWGN that gives an unpaired interval of roughly +/- 0.3 to 0.5 dB. On fading channels an unpaired interval of +/- 0.5 dB would need thousands of frames per point (the curves are that shallow), which the matrix cannot afford. So on fading channels the evidence is the paired comparison: with identical realisations the paired interval, not the unpaired one, says whether 0.5 dB is resolved. The pass rule itself is applied to the point estimates, as the plan says. A sensitivity test checks the rule can see a fault: a transmitter with its own noise 3 dB below the signal must fail it.

## Pairings and the C++ receiver slot

Today: C# to C# and C++ to C#. `Layer3Endpoints.CppReceiver` (`Endpoints.cs`) is a `MissingReceiver`, reported as "not yet available", and the pairings needing it are listed as skipped. The story that ports the C++ receiver sets it to an `ILayer3Receiver` that decodes through the C interface; the other two pairings then run with no other change, and `Agree` compares all four.

## Running

Default (ctest test `dotnet_layer3`, or by hand): `dotnet run --project tests/interop -c Release -- -trait "Category=Layer3"`. Results land in `layer3-results/default/cells/` (override with `MS110D_LAYER3_OUT`): `<cell>.curves.csv`, `<cell>.thresholds.csv`, `<cell>.md`.

Long mode: `MS110D_LAYER3_LONG=1` and `-class Ms110dInterop.Tests.Layer3.Layer3LongTests`, with `MS110D_LAYER3_SHARD=i/n`, `MS110D_LAYER3_CHANNELS`, `MS110D_LAYER3_WN`, `MS110D_LAYER3_IL`, `MS110D_LAYER3_K`, `MS110D_LAYER3_FRAMES`, `MS110D_LAYER3_THREADS`. The workflow `layer3.yml` (manual and weekly) runs 20 shards and combines the CSVs and markdown into one artifact and the job summary.
