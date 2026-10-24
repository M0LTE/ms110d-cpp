// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.Ms110d;

namespace Ms110dInterop.Tests;

/// <summary>The C++ transmitter, through the C interface, for the tests.</summary>
internal sealed class CppTransmitter : IDisposable
{
    private IntPtr _tx;

    public CppTransmitter(Ms110dTxSettings s, int maxPayloadBits)
    {
        var n = default(Native.TxSettings);
        Check(Native.TxSettingsDefault(ref n));
        n.WaveformNumber = s.WaveformNumber;
        n.Interleaver = (int)s.Interleaver;
        n.ConstraintLength = s.ConstraintLength;
        n.PreambleSuperframes = s.PreambleSuperframes;
        n.TlcBlocks = s.TlcBlocks;
        n.AppendEom = s.AppendEom ? 1 : 0;
        n.AppendEot = s.AppendEot ? 1 : 0;
        n.Amplitude = s.Amplitude;
        n.RollOff = s.RollOff;
        Check(Native.TxCreate(ref n, (uint)maxPayloadBits, out _tx));
    }

    private static void Check(int status)
    {
        if (status != Native.Ok)
        {
            throw new InvalidOperationException($"C interface status {status}: {Native.StatusString(status)}");
        }
    }

    /// <summary>The whole burst through ms110d_tx_modulate.</summary>
    public float[] ModulateOneShot(byte[] bits)
    {
        Check(Native.TxAudioLength(_tx, (uint)bits.Length, out uint need));
        var audio = new float[need];
        Check(Native.TxModulate(_tx, bits, (uint)bits.Length, audio, need, out uint written));
        if (written != need)
        {
            throw new InvalidOperationException($"modulate wrote {written}, expected {need}");
        }
        return audio;
    }

    /// <summary>The whole burst streamed: bits pushed in two pieces, audio read in chunks.</summary>
    public float[] ModulateStreamed(byte[] bits, int chunk)
    {
        Check(Native.TxBegin(_tx));
        int half = bits.Length / 2;
        Check(Native.TxPushBits(_tx, bits[..half], (uint)half));
        Check(Native.TxPushBits(_tx, bits[half..], (uint)(bits.Length - half)));
        Check(Native.TxEndPayload(_tx));
        Check(Native.TxAudioLength(_tx, (uint)bits.Length, out uint need));
        var audio = new List<float>((int)need);
        var buf = new float[chunk];
        while (true)
        {
            Check(Native.TxRead(_tx, buf, (uint)chunk, out uint got));
            if (got == 0)
            {
                break;
            }
            audio.AddRange(buf.AsSpan(0, (int)got).ToArray());
        }
        if (audio.Count != need)
        {
            throw new InvalidOperationException($"streamed {audio.Count}, expected {need}");
        }
        return [.. audio];
    }

    public void Dispose()
    {
        if (_tx != IntPtr.Zero)
        {
            Native.TxDestroy(_tx);
            _tx = IntPtr.Zero;
        }
    }
}
