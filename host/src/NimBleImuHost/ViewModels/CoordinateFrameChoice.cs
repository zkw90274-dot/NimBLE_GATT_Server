using NimBleImuHost.Protocol;

namespace NimBleImuHost.ViewModels;

/// <summary>A coordinate-frame preset paired with its Chinese UI label. Value-equality drives ComboBox selection.</summary>
public sealed record CoordinateFrameChoice(CoordinateFramePreset Preset, string Label);
