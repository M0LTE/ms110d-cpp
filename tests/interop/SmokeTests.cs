using M0LTE.Ms110d;
using Xunit;

namespace Ms110dInterop.Tests;

public class SmokeTests
{
    [Fact]
    public void AbiVersionMatchesTheHeader()
    {
        Assert.Equal(Native.ExpectedAbiVersion, Native.AbiVersion());
    }

    [Fact]
    public void CVersionMatchesTheCMakeProjectVersion()
    {
        string expected = Environment.GetEnvironmentVariable("MS110D_EXPECTED_VERSION")
            ?? throw new InvalidOperationException("MS110D_EXPECTED_VERSION is not set; run these tests through ctest.");
        Assert.Equal(Native.Ok, Native.Version(out int major, out int minor, out int patch));
        Assert.Equal(expected, $"{major}.{minor}.{patch}");
    }

    [Fact]
    public void PrecisionIsReported()
    {
        Assert.Equal(Native.Ok, Native.PrecisionBits(out int bits));
        Assert.Contains(bits, new[] { Native.PrecisionSingle, Native.PrecisionDouble });
    }

    [Fact]
    public void StatusStringsComeBack()
    {
        Assert.Equal("ok", Native.StatusString(Native.Ok));
        Assert.Equal("invalid argument", Native.StatusString(Native.ErrInvalidArgument));
    }

    [Fact]
    public void CSharpReferencePackageIsThePinnedVersion()
    {
        // The package is referenced with an exact version range taken from reference/pin.txt.
        var attr = typeof(Ms110dModulator).Assembly
            .GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .Cast<System.Reflection.AssemblyInformationalVersionAttribute>()
            .Single();
        string pinned = ReadPin("nuget_version");
        Assert.StartsWith(pinned, attr.InformationalVersion);
    }

    [Fact]
    public void CSharpModulatorLoadsAndModulates()
    {
        var modulator = new Ms110dModulator(new Ms110dTxSettings());
        var bits = new byte[] { 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0 };
        float[] audio = modulator.Modulate(bits);
        Assert.NotEmpty(audio);
        Assert.Contains(audio, s => s != 0f);
    }

    private static string ReadPin(string key)
    {
        string? dir = AppContext.BaseDirectory;
        while (dir is not null && !File.Exists(Path.Combine(dir, "reference", "pin.txt")))
        {
            dir = Path.GetDirectoryName(dir);
        }
        Assert.NotNull(dir);
        foreach (string line in File.ReadAllLines(Path.Combine(dir!, "reference", "pin.txt")))
        {
            if (line.StartsWith(key + "=", StringComparison.Ordinal))
            {
                return line[(key.Length + 1)..].Trim();
            }
        }
        throw new InvalidOperationException($"{key} not in reference/pin.txt");
    }
}
