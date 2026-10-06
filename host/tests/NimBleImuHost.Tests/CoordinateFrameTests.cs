using System.Numerics;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Tests;

/// <summary>
/// Pure-math coverage for the coordinate-frame remap (no UI, no filesystem).
/// Guards the Euler↔quaternion convention, the preset rotations, angle ranges, and the gimbal-lock clamp.
/// </summary>
public class CoordinateFrameTests
{
    private const float Deg2Rad = MathF.PI / 180f;

    // Real-hardware vector (docs/host-integration.md §2.2).
    private static readonly AttitudePacket Live = new(Roll: -0.9966f, Pitch: -2.6089f, Yaw: 58.3703f);

    [Fact]
    public void Default_IsExactPassthrough()
    {
        AttitudePacket result = CoordinateFrame.Transform(CoordinateFramePreset.Default, Live);

        // Fast-path: bit-identical, not merely close.
        Assert.Equal(Live.Roll, result.Roll);
        Assert.Equal(Live.Pitch, result.Pitch);
        Assert.Equal(Live.Yaw, result.Yaw);
    }

    [Fact]
    public void TurnAround180_LevelBoard_FlipsYaw()
    {
        AttitudePacket result = CoordinateFrame.Transform(CoordinateFramePreset.TurnAround180, new AttitudePacket(0, 0, 0));

        AssertClose(0f, result.Roll);
        AssertClose(0f, result.Pitch);
        AssertClose(180f, Math.Abs(result.Yaw));
    }

    [Fact]
    public void Inverted180_LevelBoard_FlipsRoll()
    {
        AttitudePacket result = CoordinateFrame.Transform(CoordinateFramePreset.Inverted180, new AttitudePacket(0, 0, 0));

        AssertClose(180f, Math.Abs(result.Roll));
        AssertClose(0f, result.Pitch);
        AssertClose(0f, Math.Abs(result.Yaw));
    }

    [Fact]
    public void YawLeft90_LevelBoard_ShiftsYaw()
    {
        AttitudePacket result = CoordinateFrame.Transform(CoordinateFramePreset.YawLeft90, new AttitudePacket(0, 0, 0));

        AssertClose(0f, result.Roll);
        AssertClose(0f, result.Pitch);
        AssertClose(90f, Math.Abs(result.Yaw));
    }

    [Fact]
    public void LeftThenRight90_RoundTrips()
    {
        AttitudePacket mid = CoordinateFrame.Transform(CoordinateFramePreset.YawLeft90, Live);
        AttitudePacket back = CoordinateFrame.Transform(CoordinateFramePreset.YawRight90, mid);

        AssertAngleEqual(Live.Roll, back.Roll);
        AssertClose(Live.Pitch, back.Pitch);
        AssertAngleEqual(Live.Yaw, back.Yaw);
    }

    [Fact]
    public void AllPresets_StayInDocumentedRanges()
    {
        var random = new Random(20261006);
        CoordinateFramePreset[] presets = [.. Enum.GetValues<CoordinateFramePreset>()];

        for (int i = 0; i < 5000; i++)
        {
            var raw = new AttitudePacket(
                Roll: (float)(random.NextDouble() * 360 - 180),
                Pitch: (float)(random.NextDouble() * 180 - 90),
                Yaw: (float)(random.NextDouble() * 360 - 180));

            foreach (CoordinateFramePreset preset in presets)
            {
                AttitudePacket r = CoordinateFrame.Transform(preset, raw);

                Assert.InRange(r.Roll, -180.001f, 180.001f);
                Assert.InRange(r.Pitch, -90.001f, 90.001f);
                Assert.InRange(r.Yaw, -180.001f, 180.001f);
                Assert.False(float.IsNaN(r.Roll) || float.IsNaN(r.Pitch) || float.IsNaN(r.Yaw));
            }
        }
    }

    [Fact]
    public void GimbalLock_ProducesFiniteClampedPitch()
    {
        // Display pitch ±90° is the singularity; asin() must be clamped so nothing goes NaN.
        foreach (CoordinateFramePreset preset in Enum.GetValues<CoordinateFramePreset>())
        {
            AttitudePacket r = CoordinateFrame.Transform(preset, new AttitudePacket(30f, 90f, -120f));

            Assert.False(float.IsNaN(r.Pitch));
            Assert.InRange(r.Pitch, -90.001f, 90.001f);
        }
    }

    [Fact]
    public void PresetQuaternions_AreUnit()
    {
        foreach (CoordinateFramePreset preset in Enum.GetValues<CoordinateFramePreset>())
        {
            Quaternion q = CoordinateFrame.GetRotation(preset);
            Assert.Equal(1f, q.Length(), 5);
        }
    }

    [Fact]
    public void ToDisplayQuaternion_MatchesCreateFromYawPitchRoll()
    {
        // Pins the axis convention: identity preset must equal today's on-screen orientation.
        Quaternion expected = Quaternion.CreateFromYawPitchRoll(Live.Yaw * Deg2Rad, -Live.Pitch * Deg2Rad, Live.Roll * Deg2Rad);
        Quaternion actual = CoordinateFrame.ToDisplayQuaternion(Live);

        AssertQuaternionEqual(expected, actual);
    }

    [Fact]
    public void FromDisplayQuaternion_InvertsTo()
    {
        var random = new Random(7);
        for (int i = 0; i < 2000; i++)
        {
            // Stay away from the pitch ±90° singularity where roll/yaw degenerate.
            var p = new AttitudePacket(
                Roll: (float)(random.NextDouble() * 360 - 180),
                Pitch: (float)(random.NextDouble() * 120 - 60),
                Yaw: (float)(random.NextDouble() * 360 - 180));

            AttitudePacket round = CoordinateFrame.FromDisplayQuaternion(CoordinateFrame.ToDisplayQuaternion(p));

            AssertAngleEqual(p.Roll, round.Roll);
            AssertClose(p.Pitch, round.Pitch);
            AssertAngleEqual(p.Yaw, round.Yaw);
        }
    }

    private static void AssertClose(float expected, float actual, float tolerance = 1e-3f)
    {
        float diff = MathF.Abs(expected - actual);
        Assert.True(diff < tolerance, $"expected {expected}, actual {actual}, diff {diff}");
    }

    private static void AssertAngleEqual(float expected, float actual)
    {
        float diff = MathF.Abs(((expected - actual + 180f) % 360f + 360f) % 360f - 180f);
        Assert.True(diff < 1e-2f, $"angles differ by {diff}° (expected {expected}, actual {actual})");
    }

    private static void AssertQuaternionEqual(Quaternion expected, Quaternion actual)
    {
        // q and −q encode the same rotation.
        if (Quaternion.Dot(expected, actual) < 0)
            actual = -actual;

        Assert.Equal(expected.X, actual.X, 5);
        Assert.Equal(expected.Y, actual.Y, 5);
        Assert.Equal(expected.Z, actual.Z, 5);
        Assert.Equal(expected.W, actual.W, 5);
    }
}
