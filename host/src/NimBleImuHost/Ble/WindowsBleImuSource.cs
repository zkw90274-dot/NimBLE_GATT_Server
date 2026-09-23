using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Ble;

/// <summary>
/// Windows Runtime BLE client for the NimBLE_GATT attitude characteristic.
/// Native Windows.Devices.Bluetooth — no third-party BLE stack.
/// </summary>
public sealed class WindowsBleImuSource : IBleImuSource
{
    private readonly object _gate = new();
    private readonly List<BleDiscoveredDevice> _devices = [];
    private readonly Dictionary<string, BleDiscoveredDevice> _deviceMap = new(StringComparer.OrdinalIgnoreCase);

    private BluetoothLEAdvertisementWatcher? _watcher;
    private BluetoothLEDevice? _device;
    private GattDeviceService? _service;
    private GattCharacteristic? _characteristic;
    private CancellationTokenSource? _cts;

    private ImuSourceState _state = ImuSourceState.Idle;
    private string? _status = "空闲";

    public string DisplayName => "Windows BLE";

    public ImuSourceState State
    {
        get { lock (_gate) return _state; }
    }

    public string? StatusMessage
    {
        get { lock (_gate) return _status; }
    }

    public IReadOnlyList<BleDiscoveredDevice> DiscoveredDevices
    {
        get { lock (_gate) return _devices.ToArray(); }
    }

    public event EventHandler<ImuSample>? SampleReceived;
    public event EventHandler<ImuSourceState>? StateChanged;
    public event EventHandler? DevicesChanged;

    public Task ScanAsync(TimeSpan duration, CancellationToken cancellationToken = default)
    {
        StopWatcher();

        lock (_gate)
        {
            _devices.Clear();
            _deviceMap.Clear();
        }

        DevicesChanged?.Invoke(this, EventArgs.Empty);
        SetState(ImuSourceState.Scanning, $"正在扫描 {ProtocolConstants.DeviceName} …");

        var watcher = new BluetoothLEAdvertisementWatcher
        {
            ScanningMode = BluetoothLEScanningMode.Active,
        };
        watcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(Guid.Parse(ProtocolConstants.ServiceUuid));
        watcher.Received += OnAdvertisementReceived;
        _watcher = watcher;
        watcher.Start();

        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(duration, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }

            StopWatcher();
            if (State == ImuSourceState.Scanning)
            {
                int count;
                lock (_gate) count = _devices.Count;
                SetState(ImuSourceState.Idle, count == 0 ? "未发现设备" : $"扫描结束，发现 {count} 台设备");
            }
        }, CancellationToken.None);

        return Task.CompletedTask;
    }

    public async Task ConnectAsync(string deviceId, CancellationToken cancellationToken = default)
    {
        await StopAsync().ConfigureAwait(false);
        _cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        SetState(ImuSourceState.Connecting, "正在连接…");

        try
        {
            string name = deviceId;
            lock (_gate)
            {
                if (_deviceMap.TryGetValue(deviceId, out var found))
                    name = string.IsNullOrWhiteSpace(found.Name) ? deviceId : found.Name;
            }

            BluetoothLEDevice? device = null;
            if (ulong.TryParse(deviceId.Replace(":", string.Empty), System.Globalization.NumberStyles.HexNumber, null, out ulong address))
            {
                device = await BluetoothLEDevice.FromBluetoothAddressAsync(address).AsTask(cancellationToken).ConfigureAwait(false);
            }

            if (device is null)
            {
                SetState(ImuSourceState.Faulted, "无法打开 BLE 设备（地址无效或不在范围）");
                return;
            }

            device.ConnectionStatusChanged += OnConnectionStatusChanged;
            _device = device;

            var serviceResult = await device.GetGattServicesForUuidAsync(
                    Guid.Parse(ProtocolConstants.ServiceUuid), BluetoothCacheMode.Uncached)
                .AsTask(cancellationToken).ConfigureAwait(false);

            if (serviceResult.Status != GattCommunicationStatus.Success || serviceResult.Services.Count == 0)
            {
                SetState(ImuSourceState.Faulted, "GATT 服务不可用：" + serviceResult.Status);
                return;
            }

            _service = serviceResult.Services[0];
            var charResult = await _service.GetCharacteristicsForUuidAsync(
                    Guid.Parse(ProtocolConstants.AttitudeCharacteristicUuid), BluetoothCacheMode.Uncached)
                .AsTask(cancellationToken).ConfigureAwait(false);

            if (charResult.Status != GattCommunicationStatus.Success || charResult.Characteristics.Count == 0)
            {
                SetState(ImuSourceState.Faulted, "姿态特征不可用：" + charResult.Status);
                return;
            }

            _characteristic = charResult.Characteristics[0];
            _characteristic.ValueChanged += OnValueChanged;

            var cccdStatus = await _characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue.Notify)
                .AsTask(cancellationToken).ConfigureAwait(false);

            if (cccdStatus != GattCommunicationStatus.Success)
            {
                SetState(ImuSourceState.Faulted, "订阅失败（CCCD）：" + cccdStatus);
                return;
            }

            SetState(ImuSourceState.Streaming, $"已连接 {name}，等待通知…");
        }
        catch (OperationCanceledException)
        {
            SetState(ImuSourceState.Idle, "已取消连接");
        }
        catch (Exception ex)
        {
            SetState(ImuSourceState.Faulted, "连接失败：" + ex.Message);
        }
    }

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        if (State == ImuSourceState.Streaming)
            return;

        await ScanAsync(TimeSpan.FromSeconds(4), cancellationToken).ConfigureAwait(false);

        BleDiscoveredDevice[] snapshot;
        lock (_gate) snapshot = _devices.ToArray();

        var target = snapshot.FirstOrDefault(d =>
            d.Name.Equals(ProtocolConstants.DeviceName, StringComparison.OrdinalIgnoreCase));

        if (target is null)
        {
            SetState(ImuSourceState.Faulted, $"未找到 {ProtocolConstants.DeviceName}");
            return;
        }

        await ConnectAsync(target.Id, cancellationToken).ConfigureAwait(false);
    }

    public async Task StopAsync()
    {
        StopWatcher();

        var cts = _cts;
        _cts = null;
        if (cts is not null)
        {
            await cts.CancelAsync().ConfigureAwait(false);
            cts.Dispose();
        }

        if (_characteristic is not null)
        {
            _characteristic.ValueChanged -= OnValueChanged;
            try
            {
                await _characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.None)
                    .AsTask().ConfigureAwait(false);
            }
            catch
            {
            }

            _characteristic = null;
        }

        if (_service is not null)
        {
            _service.Dispose();
            _service = null;
        }

        if (_device is not null)
        {
            _device.ConnectionStatusChanged -= OnConnectionStatusChanged;
            _device.Dispose();
            _device = null;
        }

        SetState(ImuSourceState.Idle, "已断开");
    }

    public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);

    private void OnAdvertisementReceived(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementReceivedEventArgs args)
    {
        string name = args.Advertisement?.LocalName ?? string.Empty;

        if (name.Length > 0 &&
            !name.Equals(ProtocolConstants.DeviceName, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        string id = args.BluetoothAddress.ToString("X12");
        var record = new BleDiscoveredDevice(
            id,
            string.IsNullOrWhiteSpace(name) ? ProtocolConstants.DeviceName : name,
            args.RawSignalStrengthInDBm);

        lock (_gate)
        {
            _deviceMap[id] = record;
            int idx = _devices.FindIndex(d => d.Id.Equals(id, StringComparison.OrdinalIgnoreCase));
            if (idx >= 0)
                _devices[idx] = record;
            else
                _devices.Add(record);
        }

        DevicesChanged?.Invoke(this, EventArgs.Empty);
    }

    private void OnValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args)
    {
        byte[] buffer = new byte[args.CharacteristicValue.Length];
        using (var reader = DataReader.FromBuffer(args.CharacteristicValue))
        {
            reader.ReadBytes(buffer);
        }

        if (!AttitudePacket.TryDecode(buffer, out var packet))
            return;

        // Stamped here, at the driver boundary — the render path must never re-stamp.
        SampleReceived?.Invoke(this, new ImuSample(packet, ImuSample.Now));
    }

    private void OnConnectionStatusChanged(BluetoothLEDevice sender, object args)
    {
        if (sender.ConnectionStatus == BluetoothConnectionStatus.Disconnected)
            SetState(ImuSourceState.Faulted, "连接已断开");
    }

    private void StopWatcher()
    {
        var watcher = _watcher;
        if (watcher is null)
            return;

        try
        {
            watcher.Received -= OnAdvertisementReceived;
            watcher.Stop();
        }
        catch
        {
        }

        _watcher = null;
    }

    private void SetState(ImuSourceState state, string? message)
    {
        lock (_gate)
        {
            _state = state;
            _status = message;
        }

        StateChanged?.Invoke(this, state);
    }
}
