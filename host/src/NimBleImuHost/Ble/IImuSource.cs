namespace NimBleImuHost.Ble;

public enum ImuSourceState
{
    Idle,
    Scanning,
    Connecting,
    Streaming,
    Faulted,
}

/// <summary>
/// Abstraction over the attitude stream so the UI can run against real BLE or the simulator.
/// Implementations must raise <see cref="SampleReceived"/> on any thread; UI marshals as needed.
/// </summary>
public interface IImuSource : IAsyncDisposable
{
    string DisplayName { get; }

    ImuSourceState State { get; }

    string? StatusMessage { get; }

    event EventHandler<ImuSample>? SampleReceived;

    event EventHandler<ImuSourceState>? StateChanged;

    Task StartAsync(CancellationToken cancellationToken = default);

    Task StopAsync();
}

/// <summary>BLE-backed source that scans for NimBLE_GATT and subscribes to the attitude characteristic.</summary>
public interface IBleImuSource : IImuSource
{
    IReadOnlyList<BleDiscoveredDevice> DiscoveredDevices { get; }

    event EventHandler? DevicesChanged;

    Task ScanAsync(TimeSpan duration, CancellationToken cancellationToken = default);

    Task ConnectAsync(string deviceId, CancellationToken cancellationToken = default);
}

public sealed record BleDiscoveredDevice(string Id, string Name, short Rssi);
