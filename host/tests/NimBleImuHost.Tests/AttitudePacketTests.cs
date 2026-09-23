using System.Buffers.Binary;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Tests;

/// <summary>
/// Vectors captured from real hardware (docs/host-integration.md §2.2, 2026-09-22).
/// </summary>
public class AttitudePacketTests
{
    private static readonly byte[] LiveVector =
    [
        0xA6, 0x20, 0x7F, 0xBF,
        0x78, 0xF7, 0x26, 0xC0,
        0x34, 0x7B, 0x69, 0x42,
    ];

    [Theory]
    [InlineData(0, -0.9966f)]   // roll  = -(1 + 0x7F20A6/0x800000) * 0.5
    [InlineData(4, -2.6089f)]   // pitch
    [InlineData(8, 58.3703f)]   // yaw (relative to power-on)
    public void LiveVector_DecodesToDocumentedValues(int offset, float expected)
    {
        Assert.True(AttitudePacket.TryDecode(LiveVector, out var packet));

        float actual = offset switch
        {
            0 => packet.Roll,
            4 => packet.Pitch,
            _ => packet.Yaw,
        };

        Assert.Equal(expected, actual, 3);
    }

    [Theory]
    [InlineData("27 BD 45 BF 36 AD 21 C0 FA 7A 69 42", -0.7724f, -2.5262f, 58.3701f)]
    [InlineData("FB 85 4E BF CB 23 1C C0 37 7A 69 42", -0.8067f, -2.4397f, 58.3694f)]
    [InlineData("BD FB 3C BF 8C D5 1D C0 EE 7B 69 42", -0.7382f, -2.4662f, 58.3710f)]
    public void LiveVectors_AllDocumentedFramesDecode(string frame, float roll, float pitch, float yaw)
    {
        byte[] payload = ConvertHex(frame);

        Assert.True(AttitudePacket.TryDecode(payload, out var packet));
        Assert.Equal(roll, packet.Roll, 3);
        Assert.Equal(pitch, packet.Pitch, 3);
        Assert.Equal(yaw, packet.Yaw, 3);
    }

    [Fact]
    public void BigEndian_WouldGiveWrongValues()
    {
        // Guards against silently reading the payload with the wrong byte order.
        byte[] swapped = [.. LiveVector];
        Array.Reverse(swapped, 0, 4);

        Assert.True(AttitudePacket.TryDecode(swapped, out var packet));
        Assert.False(Math.Abs(packet.Roll - (-0.9966f)) < 1e-3f);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(4)]
    [InlineData(11)]
    [InlineData(13)]
    [InlineData(16)]
    public void WrongLength_IsRejected(int length)
    {
        Assert.False(AttitudePacket.TryDecode(new byte[length], out var packet));
        Assert.Equal(default, packet);
    }

    [Fact]
    public void Encode_TriplesThroughDecode_ForRandomValues()
    {
        var random = new Random(20260923);
        for (int i = 0; i < 5000; i++)
        {
            var original = new AttitudePacket(
                Roll: (float)(random.NextDouble() * 360 - 180),
                Pitch: (float)(random.NextDouble() * 180 - 90),
                Yaw: (float)(random.NextDouble() * 360 - 180));

            byte[] payload = original.Encode();
            Assert.Equal(ProtocolConstants.PayloadLengthBytes, payload.Length);
            Assert.True(AttitudePacket.TryDecode(payload, out var decoded));

            Assert.Equal(original.Roll, decoded.Roll, 6);
            Assert.Equal(original.Pitch, decoded.Pitch, 6);
            Assert.Equal(original.Yaw, decoded.Yaw, 6);
        }
    }

    [Fact]
    public void Offsets_MatchWireLayout()
    {
        byte[] payload = new AttitudePacket(1f, 2f, 4f).Encode();

        Assert.Equal(0, ProtocolConstants.OffsetRoll);
        Assert.Equal(4, ProtocolConstants.OffsetPitch);
        Assert.Equal(8, ProtocolConstants.OffsetYaw);
        Assert.Equal(1f, BinaryPrimitives.ReadSingleLittleEndian(payload.AsSpan(0)));
        Assert.Equal(2f, BinaryPrimitives.ReadSingleLittleEndian(payload.AsSpan(4)));
        Assert.Equal(4f, BinaryPrimitives.ReadSingleLittleEndian(payload.AsSpan(8)));
    }

    private static byte[] ConvertHex(string frame)
    {
        string[] parts = frame.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        byte[] buffer = new byte[parts.Length];
        for (int i = 0; i < parts.Length; i++)
            buffer[i] = Convert.ToByte(parts[i], 16);
        return buffer;
    }
}
