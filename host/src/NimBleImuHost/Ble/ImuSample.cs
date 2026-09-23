using System.Diagnostics;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Ble;

/// <summary>
/// One attitude sample stamped on the monotonic clock at the moment it crossed the driver boundary.
/// </summary>
/// <remarks>
/// <paramref name="MonoTicks"/> is <see cref="Stopwatch"/> ticks, not wall clock: the host must never
/// re-stamp (that would fold in render latency), and <see cref="DateTimeOffset.UtcNow"/> is not monotonic.
/// Use this interval rather than a fixed dt — NOTIFY is unacknowledged and the device paces at 50/55 ms.
/// </remarks>
public readonly record struct ImuSample(AttitudePacket Attitude, long MonoTicks)
{
    public static long Now => Stopwatch.GetTimestamp();

    public static double SecondsSince(long fromTicks, long toTicks)
        => (toTicks - fromTicks) / (double)Stopwatch.Frequency;
}
