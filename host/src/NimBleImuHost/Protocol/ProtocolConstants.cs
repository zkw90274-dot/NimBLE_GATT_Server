namespace NimBleImuHost.Protocol;

/// <summary>
/// Wire-level contract for the NimBLE_GATT IMU characteristic.
/// Authoritative definition: docs/host-integration.md in the firmware repo.
/// </summary>
public static class ProtocolConstants
{
    /// <summary>Advertised device name. Prefer this over MAC (Windows randomises BLE addresses).</summary>
    public const string DeviceName = "NimBLE_GATT";

    /// <summary>Custom 128-bit primary service UUID.</summary>
    public const string ServiceUuid = "f0a1b2c3-d4e5-4f60-8a9b-000000000001";

    /// <summary>READ + NOTIFY characteristic carrying the attitude packet.</summary>
    public const string AttitudeCharacteristicUuid = "f0a1b2c3-d4e5-4f60-8a9b-000000000002";

    /// <summary>Client Characteristic Configuration Descriptor (enables NOTIFY).</summary>
    public const string CccdUuid = "00002902-0000-1000-8000-00805f9b34fb";

    /// <summary>
    /// Fixed payload: three IEEE-754 binary32 little-endian values (roll, pitch, yaw) in degrees.
    /// Must be validated before decode — protocol may grow later.
    /// </summary>
    public const int PayloadLengthBytes = 12;

    public const int OffsetRoll = 0;
    public const int OffsetPitch = 4;
    public const int OffsetYaw = 8;
}
