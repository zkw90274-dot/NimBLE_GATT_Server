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

    // 0 until the first notification of a connection has been decoded; see OnValueChanged.
    private int _firstSampleSeen;

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

    public async Task ScanAsync(TimeSpan duration, CancellationToken cancellationToken = default)
    {
        StopWatcher();

        lock (_gate)
        {
            _devices.Clear();
            _deviceMap.Clear();
        }

        DevicesChanged?.Invoke(this, EventArgs.Empty);
        SetState(ImuSourceState.Scanning, $"正在扫描 {ProtocolConstants.DeviceName} …");

        // Deliberately no AdvertisementFilter on service UUIDs. The firmware's advertising
        // packet carries flags + complete local name and nothing else (main/src/gap.c
        // start_advertising), so a UUID filter drops every packet and the scan reports
        // "no devices" even with the board in front of the antenna.
        StartWatcher();

        // The window is awaited on purpose. Callers read DiscoveredDevices as soon as this
        // returns, so a fire-and-forget scan would hand StartAsync an empty list and make the
        // "press 开始 without a manual connect" path fail every time.
        try
        {
            await Task.Delay(duration, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            StopWatcher();
        }

        if (State == ImuSourceState.Scanning)
        {
            int count;
            lock (_gate) count = _devices.Count;
            SetState(ImuSourceState.Idle, count == 0 ? "未发现设备" : $"扫描结束，发现 {count} 台设备");
        }
    }

    public async Task ConnectAsync(string deviceId, CancellationToken cancellationToken = default)
    {
        await StopAsync().ConfigureAwait(false);
        _cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        SetState(ImuSourceState.Connecting, "正在连接…");

        // Which WinRT call was in flight when it blew up - the exception itself rarely says.
        string stage = "解析地址";

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

            stage = "服务发现";

            // The by-UUID overload cannot be the first GATT call on this handle: as a fresh
            // discovery request it fails with 0x80070016 (ERROR_BAD_DEVICE), while the plain
            // enumeration of the same device answers with every service, ours included. So
            // enumerate and match the UUID locally. Measured 2026-09-23 on the MediaTek adapter.
            var serviceResult = await device.GetGattServicesAsync(BluetoothCacheMode.Cached)
                .AsTask(cancellationToken).ConfigureAwait(false);

            var serviceUuid = Guid.Parse(ProtocolConstants.ServiceUuid);
            GattDeviceService? attitudeService = serviceResult.Services
                .FirstOrDefault(s => s.Uuid == serviceUuid);

            if (serviceResult.Status != GattCommunicationStatus.Success || attitudeService is null)
            {
                SetState(ImuSourceState.Faulted,
                    $"GATT 服务不可用：{serviceResult.Status}，发现 {serviceResult.Services.Count} 个服务");
                return;
            }

            _service = attitudeService;
            stage = "特征发现";
            var charResult = await _service.GetCharacteristicsAsync(BluetoothCacheMode.Cached)
                .AsTask(cancellationToken).ConfigureAwait(false);

            GattCharacteristic? attitudeCharacteristic = charResult.Characteristics
                .FirstOrDefault(c => c.Uuid == Guid.Parse(ProtocolConstants.AttitudeCharacteristicUuid));

            if (charResult.Status != GattCommunicationStatus.Success || attitudeCharacteristic is null)
            {
                SetState(ImuSourceState.Faulted,
                    $"姿态特征不可用：{charResult.Status}，发现 {charResult.Characteristics.Count} 个特征");
                return;
            }

            _characteristic = attitudeCharacteristic;
            _characteristic.ValueChanged += OnValueChanged;

            stage = "订阅 CCCD";
            var cccdStatus = await _characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue.Notify)
                .AsTask(cancellationToken).ConfigureAwait(false);

            if (cccdStatus != GattCommunicationStatus.Success)
            {
                SetState(ImuSourceState.Faulted, "订阅失败（CCCD）：" + cccdStatus);
                return;
            }

            Interlocked.Exchange(ref _firstSampleSeen, 0);
            SetState(ImuSourceState.Streaming, $"已连接 {name}，等待通知…");
        }
        catch (OperationCanceledException)
        {
            SetState(ImuSourceState.Idle, "已取消连接");
        }
        catch (Exception ex)
        {
            // WinRT failures routinely arrive as a COMException with an empty Message, so the
            // HRESULT is the only thing that identifies them. Without it the UI reads
            // "connect failed:" and nothing else.
            string detail = Describe(ex);
            if (ex.InnerException is { } inner)
                detail += " <- " + Describe(inner);

            SetState(ImuSourceState.Faulted, $"连接失败（{stage}）：" + detail);
        }
    }

    private static string Describe(Exception ex)
        => $"{ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message}";

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
        // Match by advertised name, never by MAC: Windows hands out randomized addresses and
        // the firmware's own address is not a stable identity either. Anything unnamed is a
        // neighbouring device and stays out of the list.
        string name = args.Advertisement?.LocalName ?? string.Empty;
        if (!name.Equals(ProtocolConstants.DeviceName, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        string id = args.BluetoothAddress.ToString("X12");
        var record = new BleDiscoveredDevice(id, name, args.RawSignalStrengthInDBm);

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

        // The connect path can only promise "subscribed"; whether packets actually arrive is
        // first proven here. Leaving the status at "waiting" made a live stream look stalled.
        if (Interlocked.Exchange(ref _firstSampleSeen, 1) == 0)
            SetState(ImuSourceState.Streaming, $"数据流运行中（{ProtocolConstants.DeviceName}）");

        // Stamped here, at the driver boundary — the render path must never re-stamp.
        SampleReceived?.Invoke(this, new ImuSample(packet, ImuSample.Now));
    }

    private void OnConnectionStatusChanged(BluetoothLEDevice sender, object args)
    {
        if (sender.ConnectionStatus == BluetoothConnectionStatus.Disconnected)
            SetState(ImuSourceState.Faulted, "连接已断开");
    }

    private void StartWatcher()
    {
        var watcher = new BluetoothLEAdvertisementWatcher
        {
            ScanningMode = BluetoothLEScanningMode.Active,
        };
        watcher.Received += OnAdvertisementReceived;
        _watcher = watcher;
        watcher.Start();
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
