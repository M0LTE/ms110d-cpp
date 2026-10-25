# Shadow receiver: both decoders on every slot

The shadow receiver (docs/plan.md, "Live side by side on a mailcast receiver") runs the pinned C# modem and this library's C++ receiver on the same real HF audio and reports where they differ. This page is how it is built and run. It covers stories 38 (the dual decoder) and 40 (the daily report) of epic 36.

## How it differs from the plan

The plan says one live audio stream is split to both demodulators. What runs is simpler and gives the same guarantee: a container on the LAN records every GB7RDG mailcast slot to disk (docs/dev/corpus.md describes the recordings), and after each slot is filed both decoders are run offline over that slot's `audio.wav`. Both decoders hear the identical samples, so any difference is the modem and not the radio path, which is what the split stream was for. It is also easier to replay: the audio is kept, every disagreement can be rerun at will, and nothing about the live receiver or the recorder changes. A live split would add real time scheduling and a second feed to get wrong, and would gain nothing.

The recordings are equivalent to, not bit-identical with, what the live mailcast receiver heard (both demodulate the same IQ separately). The live receiver's own decode log is kept beside the audio but is not part of the comparison.

## The pieces

- `tools/ms110d-decode/` is the C++ side: a command line tool around the public `Receiver`, built by CMake with `-DMS110D_BUILD_TOOLS=ON` (off by default, so library consumers never build it). It reads a mono 16-bit or float WAV at any rate of 9600 Hz or more, resamples to 9600 Hz, runs the receiver and prints one JSON object per line.
- `tools/csharp-vectors` mode `shadow` is the C# twin: `shadow decode WAV` runs the pinned M0LTE.Ms110d over a 9600 Hz WAV and prints the same events. `shadow finish JSONL` is the second pass for the C++ output, see below.
- `scripts/shadow/compare.py` pairs the two outputs burst by burst and writes the slot report and the daily summary.
- `scripts/shadow/ms110d-shadow-decode`, `systemd/` and `install.sh` run it all on the recording container.

## Same audio, exactly

Both decoders read one resampled file. The C++ tool resamples the recording to 9600 Hz once (`--write-resampled FILE`) and the C# twin reads that file. The resampler is the one in `scripts/corpus/curate.py` (4/5 polyphase, 801-tap Kaiser, beta 8, 4400 Hz cutoff at the 48 kHz intermediate rate, double precision, rounded to 16 bits), written again in C++ because the container has no numpy; it matches the Python sample for sample. Checked on a whole 15 minute slot (8,640,000 samples): no sample differs. The other rates the tool accepts use the same method with the up and down factors worked out from the rate.

Both decoders are fed as the corpus tools feed the C# (docs/dev/corpus.md): chunks of 96 samples, then three seconds of silence, then a terminate if a burst is still open.

## The JSON events

One object per line, each with `ev` and `decoder` (`cxx` or `cs`):

| `ev` | Fields |
| --- | --- |
| `burst_started` | `burst`, `sample`, `wn`, `il`, `k`, `cfo_hz` |
| `burst_refused` | the same minus `cfo_hz`; C++ only, and only if `--max-interleaver` is set below Long |
| `block` | `burst`, `index`, `sample`, `bits`, `good_frames` (IL2P frames that completed in this block) |
| `burst_completed` | `burst`, `sample`, `start_sample`, `wn`, `il`, `k`, `blocks`, `reason`, `payload_bits`, `snr_db`, `bytes`, `sha256`, `good_frames`, `frames_sha256`, `corrected`, `crc_failures`, `rs_failures`, `error_free`, `cpu_ms` |
| `summary` | `version`, `audio_s`, `bursts`, `cpu_ms_total`, `cpu_ms_in_bursts`, `cpu_ms_search` |

Positions are input samples at 9600 Hz. The C++ numbers are exact; the C# numbers are good to the 96 sample chunk. `sha256` is the corpus payload hash (bits cut to `payload_bits`, packed MSB first, zero padded) and `frames_sha256` the corpus frames hash, so both can be checked against `manifest.json`.

The IL2P+CRC deframer exists only in C# (M0LTE.Il2p), so the C++ tool prints each block's bits (`bits_hex`, packed) and `shadow finish` runs the same deframer over them and adds the payload hash and frame counts, dropping `bits_hex`. The C# twin uses that very code path on its own blocks, so the framing step cannot differ between the two.

Processor time (`cpu_ms`) is the time of the calling thread spent inside the receiver from the moment it leaves the search until the burst ends, preamble reading included, taken around every 96 sample call; both tools count it the same way. The time spent searching for a preamble is in `cpu_ms_search`. `burst_started` and `start_sample` are the lock (the C++ `BurstStarted`, the C# state `Tracking`) in both tools; the corpus tool's burst start is the earlier moment of leaving the search, which only matters for cutting bursts out. The C# figures include JIT warm-up and a managed runtime, so read the ratio, not the absolute number.

## The comparison

For each burst the report says whether it was decoded by both, by C# only, by C++ only or by neither (a decode means at least one good IL2P frame), or whether only one decoder locked on it. It lists good frames, end reason and block count for each, the processor time of each, and the verdict of the layer 4 rule (docs/dev/corpus.md): waveform, interleaver and constraint length equal, end reason and block count equal when the C# has a good frame, at most one good frame short per burst and 2 percent over the set, and the payload equal on an error-free burst. A burst only the C# locked on counts as all its good frames short, as in `CorpusTests`. Frames that fail their CRC are never counted, so the rule's "every frame decoded passes its CRC" holds by construction.

The daily summary adds per slot and per waveform tables and a list of differences to look at: any one-sided decode, any burst one decoder missed that had good frames, and any rule problem. The plan's pass mark is a fortnight with none of those unexplained.

SNR is measured from the audio, not taken from either receiver (neither exposes one): the burst's power from lock to end over the slot's quiet floor, the corpus estimate, computed by the same code in both tools. The report shows it per burst and groups the daily summary by SNR band.

## The container

A Debian container records the slots (a receiver plus a raw audio capture, a keeper that cuts each slot out as `slots/<UTC>/audio.wav`, `decode.log`, `meta.json`). Beside that, `scripts/shadow/install.sh` installs:

- `/opt/ms110d-shadow/bin/ms110d-decode`, built on the container with cmake and g++, and the C# twin published self-contained (`dotnet publish tools/csharp-vectors -c Release -r linux-x64 --self-contained -p:InvariantGlobalization=true`) so the container needs no .NET;
- `/usr/local/sbin/ms110d-shadow-decode` with `pending`, `slot DIR`, `all` and `daily [DATE]`;
- a timer that runs `pending` a few minutes after each run of the slot keeper, at low priority and with a memory cap so it never competes with the recorder, and a timer that writes yesterday's report at 00:40 UTC.

A slot that fails (a decoder crash, say) is retried by later runs up to three times; then it is given up on, `failed.json` in the slot directory keeps the reason, and the daily report lists every filed slot with no comparison, with why (no audio, given up, not decoded yet). `slot` and `all` retry given-up slots. Each run logs one line per slot ("done in N s, M bursts" or the failure) and a total.

Per slot it writes `cxx.jsonl`, `cs.jsonl` and `compare.md` next to `audio.wav`, `compare.md` last so its presence marks the slot done, and the daily timer writes `reports/<date>.md`. Rerun a slot with `ms110d-shadow-decode slot DIR`; rerun everything after a rebuild with `ms110d-shadow-decode all`. To move to a new C++ or C# build, rerun `install.sh` from the new checkout and then `all`.

## First results

The first three slots (2026-10-24, 14:00 to 16:00 UTC), 42 bursts, all WN 3 and WN 4:

- 28 bursts decoded by both, none by one decoder only, 14 by neither (weak WN 3 bursts and locks on noise). Both locked on the same 42 bursts.
- 109 good IL2P frames from each decoder. The payload hash, end reason and block count are identical on all 42 paired bursts, including the ones with no good frame.
- No layer 4 rule problem and no difference to look at.
- Processor time over 45 minutes of audio: about 91 s for the C# and 71 s for the C++ on the container's cores (the C# includes JIT warm-up); inside bursts 67 s against 43 s.

These slots are also the source of corpus-v1, so this agrees with the corpus result by a second route: whole slots, with the C++ finding the bursts itself rather than being handed cut files.
