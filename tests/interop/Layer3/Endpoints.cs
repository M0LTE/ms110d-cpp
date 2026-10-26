// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.Ms110d;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>A transmitter under test: the same payload and settings in, 9600 Hz audio out.</summary>
public interface ILayer3Transmitter
{
    string Name { get; }

    float[] Modulate(Ms110dTxSettings settings, byte[] payload);
}

/// <summary>What a receiver made of one burst. A null <see cref="Payload"/> means nothing was decoded.</summary>
public sealed record Layer3Decode(byte[]? Payload);

/// <summary>
/// A receiver under test. A receiver that is not available is reported as such and every pairing that needs it
/// is skipped (see docs/dev/layer3.md).
/// </summary>
public interface ILayer3Receiver
{
    string Name { get; }

    /// <summary>False while the receiver is only a placeholder; the reason says what is missing.</summary>
    bool IsAvailable { get; }

    string UnavailableReason { get; }

    /// <summary>Decodes one burst of noisy audio. Must be safe to call from several threads at once.</summary>
    Layer3Decode Decode(float[] audio);
}

/// <summary>
/// The C# reference modulator. Since 0.3.0 it sends the D.5.2.2 standard probe pattern, the same as the C++ transmitter.
/// </summary>
public sealed class CSharpTransmitter : ILayer3Transmitter
{
    public string Name => "csharp";

    public float[] Modulate(Ms110dTxSettings settings, byte[] payload) => new Ms110dModulator(settings).Modulate(payload);
}

/// <summary>The C++ transmitter through the C interface.</summary>
public sealed class CppTransmitterEndpoint : ILayer3Transmitter
{
    public string Name => "cpp";

    public float[] Modulate(Ms110dTxSettings settings, byte[] payload)
    {
        using var tx = new CppTransmitter(settings, payload.Length);
        return tx.ModulateOneShot(payload);
    }
}

/// <summary>The C# reference demodulator: one burst per call, a fresh demodulator each time.</summary>
public sealed class CSharpReceiver : ILayer3Receiver
{
    public string Name => "csharp";

    public bool IsAvailable => true;

    public string UnavailableReason => "";

    public Layer3Decode Decode(float[] audio)
    {
        var demod = new Ms110dDemodulator();
        Ms110dBurst? burst = null;
        demod.BurstCompleted += b => burst ??= b;
        demod.Process(audio);
        demod.Process(new float[6000]);
        return new Layer3Decode(burst?.PayloadBits);
    }
}

/// <summary>
/// The C++ receiver through the C interface, decoding exactly as <see cref="CSharpReceiver"/> does: the audio
/// then 6000 zero samples, the first burst completed. One receiver per thread, reset between bursts (each is
/// sized for every interleaver, about 61 MB, so they are not made per call).
/// </summary>
public sealed class CppReceiverEndpoint : ILayer3Receiver
{
    private readonly ThreadLocal<CppReceiver> _rx = new(() => new CppReceiver(), trackAllValues: false);

    public string Name => "cpp";

    public bool IsAvailable => true;

    public string UnavailableReason => "";

    public Layer3Decode Decode(float[] audio)
    {
        CppReceiver rx = _rx.Value!;
        rx.Reset();
        var bursts = rx.Process(audio);
        bursts.AddRange(rx.Process(new float[6000]));
        return new Layer3Decode(bursts.Count > 0 ? bursts[0].PayloadBits : null);
    }
}

/// <summary>A receiver slot with nothing in it (kept for the harness's own tests of an unavailable receiver).</summary>
public sealed class MissingReceiver(string name, string reason) : ILayer3Receiver
{
    public string Name => name;

    public bool IsAvailable => false;

    public string UnavailableReason => reason;

    public Layer3Decode Decode(float[] audio) => throw new NotSupportedException($"receiver {name} is not available: {reason}");
}

/// <summary>
/// The endpoints the harness knows about: both transmitters and both receivers, so all four pairings run.
/// </summary>
public static class Layer3Endpoints
{
    public static readonly ILayer3Transmitter CSharpTx = new CSharpTransmitter();
    public static readonly ILayer3Transmitter CppTx = new CppTransmitterEndpoint();
    public static readonly ILayer3Receiver CSharpRx = new CSharpReceiver();

    /// <summary>The C++ receiver through the C interface.</summary>
    public static ILayer3Receiver CppReceiver { get; set; } = new CppReceiverEndpoint();

    public static IReadOnlyList<ILayer3Transmitter> Transmitters => [CSharpTx, CppTx];

    public static IReadOnlyList<ILayer3Receiver> Receivers => [CSharpRx, CppReceiver];
}

/// <summary>
/// A receiver that is deliberately a little worse than the one it wraps, to prove the pass rule can see a
/// known loss: it adds white noise to the incoming audio, a chosen fraction of the channel noise already there
/// (measured from the noise-only lead-in, so it tracks the SNR of each point). Adding the fraction f costs
/// 10 log10(1 + f) dB, so 0.2 dB is f = 0.047 and 1 dB is f = 0.259. The noise is seeded from the run seed and a
/// fingerprint of the audio, so different runs draw different noise and the same run repeats.
/// </summary>
public sealed class ExtraNoiseReceiver(ILayer3Receiver inner, string name, double relativePower, int runSeed) : ILayer3Receiver
{
    public string Name => name;

    public bool IsAvailable => inner.IsAvailable;

    public string UnavailableReason => inner.UnavailableReason;

    /// <summary>The fraction of extra noise power that costs <paramref name="db"/> dB.</summary>
    public static double FractionFor(double db) => Math.Pow(10, db / 10) - 1;

    public Layer3Decode Decode(float[] audio)
    {
        const int leadIn = 2000; // the channel adds 2400 noise-only samples first
        double v = 0;
        for (int i = 0; i < leadIn; i++)
        {
            v += (double)audio[i] * audio[i];
        }

        double sigma = Math.Sqrt(v / leadIn * relativePower);
        ulong h = 14695981039346656037UL ^ (ulong)(uint)runSeed;
        for (int i = 0; i < audio.Length; i += 97)
        {
            h = (h ^ (uint)BitConverter.SingleToInt32Bits(audio[i])) * 1099511628211UL;
        }

        var rng = new Random((int)(h ^ (h >> 32)));
        var o = new float[audio.Length];
        for (int i = 0; i < o.Length; i++)
        {
            double g = Math.Sqrt(-2 * Math.Log(1 - rng.NextDouble())) * Math.Cos(2 * Math.PI * rng.NextDouble());
            o[i] = audio[i] + (float)(sigma * g);
        }

        return inner.Decode(o);
    }
}
