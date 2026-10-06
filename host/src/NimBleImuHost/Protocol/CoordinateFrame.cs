using System.Numerics;

namespace NimBleImuHost.Protocol;

/// <summary>
/// Named coordinate-frame re-orientations. <see cref="Default"/> is an exact passthrough.
/// The rest are fixed rotations about the display world axes (X=lateral/right, Y=up, Z=forward/nose).
/// </summary>
public enum CoordinateFramePreset
{
    Default,
    YawLeft90,
    YawRight90,
    TurnAround180,
    Inverted180,
    SideRoll90,
}

/// <summary>
/// Re-orients a decoded attitude into a selected coordinate frame.
///
/// The 3D view maps the packet as roll→Z(+), pitch→X(−), yaw→Y(+), applied roll→pitch→yaw, which is the
/// column-vector rotation R_Y(yaw)·R_X(−pitch)·R_Z(roll). <see cref="ToDisplayQuaternion"/> reproduces that
/// exactly, so the identity preset round-trips bit-for-bit and the transformed Euler angles fed back into the
/// view render precisely the re-oriented frame — keeping the 3D model and the numeric readouts in agreement.
///
/// The device only sends Euler angles, so near display-pitch ±90° roll/yaw degenerate (gimbal lock, see
/// docs/imu.md §4.10). No preset here drives a level board onto that singularity, and <see cref="Default"/>
/// bypasses the round-trip entirely.
/// </summary>
public static class CoordinateFrame
{
    private const float Deg2Rad = MathF.PI / 180f;
    private const float Rad2Deg = 180f / MathF.PI;

    /// <summary>The fixed rotation for a preset, as a unit quaternion.</summary>
    public static Quaternion GetRotation(CoordinateFramePreset preset) => preset switch
    {
        CoordinateFramePreset.YawLeft90 => Quaternion.CreateFromAxisAngle(Vector3.UnitY, 90f * Deg2Rad),
        CoordinateFramePreset.YawRight90 => Quaternion.CreateFromAxisAngle(Vector3.UnitY, -90f * Deg2Rad),
        CoordinateFramePreset.TurnAround180 => Quaternion.CreateFromAxisAngle(Vector3.UnitY, 180f * Deg2Rad),
        CoordinateFramePreset.Inverted180 => Quaternion.CreateFromAxisAngle(Vector3.UnitZ, 180f * Deg2Rad),
        CoordinateFramePreset.SideRoll90 => Quaternion.CreateFromAxisAngle(Vector3.UnitZ, 90f * Deg2Rad),
        _ => Quaternion.Identity,
    };

    /// <summary>Re-orient a raw attitude into the selected frame. <see cref="CoordinateFramePreset.Default"/> is an exact passthrough.</summary>
    public static AttitudePacket Transform(CoordinateFramePreset preset, in AttitudePacket raw)
    {
        if (preset == CoordinateFramePreset.Default)
            return raw;

        Quaternion q = GetRotation(preset) * ToDisplayQuaternion(raw);
        return FromDisplayQuaternion(q);
    }

    /// <summary>Build the display-frame quaternion, mirroring Attitude3DView's axis mapping (note the pitch negation).</summary>
    internal static Quaternion ToDisplayQuaternion(in AttitudePacket p) =>
        Quaternion.CreateFromAxisAngle(Vector3.UnitY, p.Yaw * Deg2Rad) *
        Quaternion.CreateFromAxisAngle(Vector3.UnitX, -p.Pitch * Deg2Rad) *
        Quaternion.CreateFromAxisAngle(Vector3.UnitZ, p.Roll * Deg2Rad);

    /// <summary>Exact inverse of <see cref="ToDisplayQuaternion"/>: decompose R_Y(yaw)·R_X(pitchX)·R_Z(rollZ) back to a packet.</summary>
    internal static AttitudePacket FromDisplayQuaternion(Quaternion q)
    {
        q = Quaternion.Normalize(q);
        float w = q.W, x = q.X, y = q.Y, z = q.Z;

        // Clamp guards asin() against float drift past ±1 (would yield NaN at the singularity).
        float pitchX = MathF.Asin(Math.Clamp(2f * (w * x - y * z), -1f, 1f));
        float rollZ = MathF.Atan2(2f * (x * y + w * z), 1f - 2f * (x * x + z * z));
        float yawY = MathF.Atan2(2f * (x * z + w * y), 1f - 2f * (x * x + y * y));

        // The -Pitch here cancels the -Pitch in ToDisplayQuaternion, so identity round-trips exactly.
        return new AttitudePacket(rollZ * Rad2Deg, -pitchX * Rad2Deg, yawY * Rad2Deg);
    }
}
