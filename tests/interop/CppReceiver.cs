// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Runtime.InteropServices;
using M0LTE.Ms110d;

namespace Ms110dInterop.Tests;

/// <summary>One burst as the C++ receiver reported it through the C interface.</summary>
internal sealed record CppBurst(int Wn, Ms110dInterleaverKind Il, int K, int Blocks, Ms110dBurstEndReason Reason,
    byte[] PayloadBits, List<byte[]> BlockBits, long StartedAt, long EndedAt);

/// <summary>
/// The C++ receiver through the C interface (ms110d_rx_*), for the tests: audio in, bursts out, in the
/// shape of the C# events. Not thread safe; use one per thread.
/// </summary>
internal sealed class CppReceiver : IDisposable
{
    private IntPtr _rx;
    private readonly byte[] _bits = new byte[49152];
    private int _wn = -1, _k;
    private Ms110dInterleaverKind _il;
    private long _started = -1;
    private List<byte[]> _blocks = [];

    /// <summary>Called with every block's bits as it is decoded (for an IL2P deframer, say).</summary>
    public Action<byte[]>? BlockDecoded { get; set; }

    static CppReceiver()
    {
        // Bind the entry points once, on one thread: lazy binding of a P/Invoke through the resolver
        // from several threads at once has crashed the runtime (see Native.BindAll).
        var o = default(Native.RxOptions);
        _ = Native.RxOptionsDefault(ref o);
        unsafe
        {
            _ = Native.RxProcess(IntPtr.Zero, null, 0, out _);
            var e = default(Native.RxEvent);
            _ = Native.RxNextEvent(IntPtr.Zero, ref e, null, 0, null, 0);
        }
        _ = Native.RxTerminate(IntPtr.Zero);
        _ = Native.RxReset(IntPtr.Zero);
        _ = Native.RxState(IntPtr.Zero, out _);
        _ = Native.RxCreate(ref o, out IntPtr made);
        Native.RxDestroy(made);
    }

    public CppReceiver(Ms110dInterleaverKind maxInterleaver = Ms110dInterleaverKind.Long)
    {
        var o = default(Native.RxOptions);
        Check(Native.RxOptionsDefault(ref o));
        o.MaxInterleaver = (int)maxInterleaver;
        Check(Native.RxCreate(ref o, out _rx));
    }

    private static void Check(int status)
    {
        if (status != Native.Ok)
        {
            throw new InvalidOperationException($"C interface status {status}: {Native.StatusString(status)}");
        }
    }

    public void Reset()
    {
        Check(Native.RxReset(_rx));
        _wn = -1;
        _started = -1;
        _blocks = [];
    }

    public void Terminate() => Check(Native.RxTerminate(_rx));

    /// <summary>True from an accepted preamble peak until the burst ends (the C# State != Searching).</summary>
    public bool Busy
    {
        get
        {
            Check(Native.RxState(_rx, out int state));
            return state != 0;
        }
    }

    /// <summary>Feeds audio (any length) and returns the bursts that ended in it.</summary>
    public unsafe List<CppBurst> Process(ReadOnlySpan<float> audio)
    {
        var done = new List<CppBurst>();
        fixed (float* p = audio)
        fixed (byte* bits = _bits)
        {
            int at = 0;
            while (at < audio.Length)
            {
                Check(Native.RxProcess(_rx, p + at, (uint)(audio.Length - at), out uint used));
                at += (int)used;
                while (true)
                {
                    var e = new Native.RxEvent { StructSize = (uint)Marshal.SizeOf<Native.RxEvent>() };
                    Check(Native.RxNextEvent(_rx, ref e, bits, (uint)_bits.Length, null, 0));
                    if (e.Kind == Native.RxEventNone)
                    {
                        break;
                    }
                    switch (e.Kind)
                    {
                        case Native.RxEventBurstStarted:
                            _wn = e.WaveformNumber;
                            _il = (Ms110dInterleaverKind)e.Interleaver;
                            _k = e.ConstraintLength;
                            _started = e.Sample;
                            _blocks = [];
                            break;
                        case Native.RxEventBlockDecoded:
                            byte[] b = _bits.AsSpan(0, (int)e.Length).ToArray();
                            _blocks.Add(b);
                            BlockDecoded?.Invoke(b);
                            break;
                        case Native.RxEventBurstCompleted:
                            var payload = new byte[e.PayloadBits];
                            int n = 0;
                            foreach (byte[] blk in _blocks)
                            {
                                int take = Math.Min(blk.Length, payload.Length - n);
                                if (take <= 0)
                                {
                                    break;
                                }
                                blk.AsSpan(0, take).CopyTo(payload.AsSpan(n));
                                n += take;
                            }
                            done.Add(new CppBurst(_wn, _il, _k, e.Blocks, (Ms110dBurstEndReason)e.EndReason, payload, _blocks,
                                _started, e.Sample));
                            _wn = -1;
                            _blocks = [];
                            break;
                    }
                }
            }
        }
        return done;
    }

    public void Dispose()
    {
        if (_rx != IntPtr.Zero)
        {
            Native.RxDestroy(_rx);
            _rx = IntPtr.Zero;
        }
    }
}
