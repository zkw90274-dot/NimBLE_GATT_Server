using System.IO;
using System.Text.Json;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Settings;

/// <summary>
/// Minimal JSON settings store. Writes to %LocalAppData% so it works even when the portable single-file exe
/// lives in a read-only folder, and is per-user. Every method is exception-safe: a missing, corrupt, or
/// unwritable file must never crash the render loop — it silently falls back to defaults.
/// </summary>
public static class SettingsStore
{
    private static readonly string DefaultDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NimBleImuHost");

    private static readonly string DefaultPath = Path.Combine(DefaultDir, "settings.json");

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public static CoordinateFramePreset LoadCoordinateFrame() => LoadCoordinateFrame(DefaultPath);

    public static void SaveCoordinateFrame(CoordinateFramePreset preset) => SaveCoordinateFrame(preset, DefaultPath);

    internal static CoordinateFramePreset LoadCoordinateFrame(string path) =>
        Enum.TryParse(Load(path).CoordinateFrame, out CoordinateFramePreset preset)
            ? preset
            : CoordinateFramePreset.Default;

    internal static void SaveCoordinateFrame(CoordinateFramePreset preset, string path)
    {
        AppSettings settings = Load(path);
        settings.CoordinateFrame = preset.ToString();
        Save(path, settings);
    }

    internal static AppSettings Load(string path)
    {
        try
        {
            if (!File.Exists(path))
                return new AppSettings();
            return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path)) ?? new AppSettings();
        }
        catch
        {
            return new AppSettings(); // missing / corrupt / IO error => defaults
        }
    }

    internal static void Save(string path, AppSettings settings)
    {
        try
        {
            string? dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
            File.WriteAllText(path, JsonSerializer.Serialize(settings, JsonOptions));
        }
        catch
        {
            // Non-fatal: a read-only or full profile must not take down the app.
        }
    }
}
