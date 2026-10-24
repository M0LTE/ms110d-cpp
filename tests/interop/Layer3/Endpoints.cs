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
/// A receiver under test. The C++ receiver does not exist yet: <see cref="Layer3Endpoints.CppReceiver"/> is
/// the slot a later story fills (see docs/dev/layer3.md). A receiver that is not available is reported as
/// such and every pairing that needs it is skipped.
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

/// <summary>The C# reference modulator.</summary>
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

/// <summary>The slot for the C++ receiver, which is being ported. Replace <see cref="Layer3Endpoints.CppReceiver"/>.</summary>
public sealed class MissingReceiver(string name, string reason) : ILayer3Receiver
{
    public string Name => name;

    public bool IsAvailable => false;

    public string UnavailableReason => reason;

    public Layer3Decode Decode(float[] audio) => throw new NotSupportedException($"receiver {name} is not available: {reason}");
}

/// <summary>
/// The endpoints the harness knows about. The C++ receiver lands here: when it exists, assign a class
/// implementing <see cref="ILayer3Receiver"/> to <see cref="CppReceiver"/> (one line) and every pairing
/// that needs it starts running, with no other change.
/// </summary>
public static class Layer3Endpoints
{
    public static readonly ILayer3Transmitter CSharpTx = new CSharpTransmitter();
    public static readonly ILayer3Transmitter CppTx = new CppTransmitterEndpoint();
    public static readonly ILayer3Receiver CSharpRx = new CSharpReceiver();

    /// <summary>Not yet available. Filled by the C++ receiver stories under epic 23.</summary>
    public static ILayer3Receiver CppReceiver { get; set; } = new MissingReceiver("cpp", "the C++ receiver is not ported yet");

    public static IReadOnlyList<ILayer3Transmitter> Transmitters => [CSharpTx, CppTx];

    public static IReadOnlyList<ILayer3Receiver> Receivers => [CSharpRx, CppReceiver];
}
