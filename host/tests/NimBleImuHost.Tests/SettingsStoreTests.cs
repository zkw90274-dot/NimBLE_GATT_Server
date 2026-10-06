using System.IO;
using NimBleImuHost.Protocol;
using NimBleImuHost.Settings;

namespace NimBleImuHost.Tests;

/// <summary>
/// Persistence robustness: a missing, corrupt, or unknown-value file must fall back to Default, and a saved
/// preset must round-trip. Uses the internal path-injectable overloads against a temp file.
/// </summary>
public sealed class SettingsStoreTests : IDisposable
{
    private readonly string _path = Path.Combine(Path.GetTempPath(), $"nimble-settings-{Guid.NewGuid():N}.json");

    [Fact]
    public void Load_MissingFile_ReturnsDefault()
    {
        Assert.Equal(CoordinateFramePreset.Default, SettingsStore.LoadCoordinateFrame(_path));
    }

    [Fact]
    public void Load_CorruptJson_ReturnsDefault()
    {
        File.WriteAllText(_path, "{ this is not json ");

        Assert.Equal(CoordinateFramePreset.Default, SettingsStore.LoadCoordinateFrame(_path));
    }

    [Fact]
    public void Load_UnknownFrameName_ReturnsDefault()
    {
        File.WriteAllText(_path, """{"Version":1,"CoordinateFrame":"NoSuchPreset"}""");

        Assert.Equal(CoordinateFramePreset.Default, SettingsStore.LoadCoordinateFrame(_path));
    }

    [Theory]
    [InlineData(CoordinateFramePreset.TurnAround180)]
    [InlineData(CoordinateFramePreset.Inverted180)]
    [InlineData(CoordinateFramePreset.YawLeft90)]
    [InlineData(CoordinateFramePreset.Default)]
    public void SaveThenLoad_RoundTrips(CoordinateFramePreset preset)
    {
        SettingsStore.SaveCoordinateFrame(preset, _path);

        Assert.Equal(preset, SettingsStore.LoadCoordinateFrame(_path));
    }

    public void Dispose()
    {
        try { File.Delete(_path); } catch { /* best effort */ }
    }
}
