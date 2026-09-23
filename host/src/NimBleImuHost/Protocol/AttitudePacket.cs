using System.Buffers.Binary;

namespace NimBleImuHost.Protocol;

/// <summary>
/// Decoded attitude sample. Units are degrees.
/// yaw is relative to power-on and drifts (~0.1 deg/min) — it is not a compass heading.
/// </summary>
public readonly record struct AttitudePacket(float Roll, float Pitch, float Yaw)
{
    /// <summary>
    /// Decode a 12-byte little-endian float32 triple (roll, pitch, yaw).
    /// Returns false when the buffer is not exactly <see cref="ProtocolConstants.PayloadLengthBytes"/> long.
    /// </summary>
    public static bool TryDecode(ReadOnlySpan<byte> payload, out AttitudePacket packet)
    {
        if (payload.Length != ProtocolConstants.PayloadLengthBytes)
        {
            packet = default;
            return false;
        }

        float roll = BinaryPrimitives.ReadSingleLittleEndian(payload.Slice(ProtocolConstants.OffsetRoll, 4));
        float pitch = BinaryPrimitives.ReadSingleLittleEndian(payload.Slice(ProtocolConstants.OffsetPitch, 4));
        float yaw = BinaryPrimitives.ReadSingleLittleEndian(payload.Slice(ProtocolConstants.OffsetYaw, 4));
        packet = new AttitudePacket(roll, pitch, yaw);
        return true;
    }

    /// <summary>Encode for tests and the simulator. Inverse of <see cref="TryDecode"/>.</summary>
    public byte[] Encode()
    {
        byte[] buffer = new byte[ProtocolConstants.PayloadLengthBytes];
        BinaryPrimitives.WriteSingleLittleEndian(buffer.AsSpan(ProtocolConstants.OffsetRoll, 4), Roll);
        BinaryPrimitives.WriteSingleLittleEndian(buffer.AsSpan(ProtocolConstants.OffsetPitch, 4), Pitch);
        BinaryPrimitives.WriteSingleLittleEndian(buffer.AsSpan(ProtocolConstants.OffsetYaw, 4), Yaw);
        return buffer;
    }
}
