namespace NimBleImuHost.Settings;

/// <summary>
/// Persisted user preferences. Stored as JSON under %LocalAppData%\NimBleImuHost\settings.json.
/// The preset is kept as its enum-name string so reordering the enum never corrupts a saved file.
/// </summary>
public sealed class AppSettings
{
    public int Version { get; set; } = 1;
    public string CoordinateFrame { get; set; } = "Default";
}
