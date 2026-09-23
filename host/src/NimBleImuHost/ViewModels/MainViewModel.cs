using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using NimBleImuHost.Ble;

namespace NimBleImuHost.ViewModels;

/// <summary>
/// Acquisition side of the pipeline: samples arrive on the source thread and are only pushed into a
/// lock-free ring here. Everything the user sees is produced by <see cref="ReportFrame"/>, which the
/// render tick calls on the UI thread.
/// </summary>
/// <remarks>
/// The producer never touches DependencyProperty-backed state and never marshals to the dispatcher:
/// one <c>BeginInvoke</c> per sample is what caps a naive host near a few hundred Hz, because the
/// dispatcher queue becomes the bottleneck instead of the drawing.
/// </remarks>
public sealed class MainViewModel : INotifyPropertyChanged, IAsyncDisposable
{
    private const double StatsWindowSeconds = 0.5;

    private readonly IBleImuSource _ble;
    private readonly SimulatedImuSource _sim;
    private readonly AttitudeRingBuffer _ring = new();
    private IImuSource _active;

    private long _ingestTotal;          // producer thread writes: Interlocked only
    private long _windowIngestBase;
    private long _windowStartTicks;
    private int _windowRenders;

    private string _status = "就绪";
    private bool _useBle;
    private float _roll;
    private float _pitch;
    private float _yaw;
    private double _ingestHz;
    private double _renderFps;
    private long _dropped;
    private bool _isBusy;
    private ulong _generation;
    private BleDiscoveredDevice? _selectedDevice;

    public MainViewModel()
    {
        _ble = new WindowsBleImuSource();
        _sim = new SimulatedImuSource();
        _active = _sim;

        _ble.DevicesChanged += (_, _) => RefreshDevices();
        Hook(_ble);
        Hook(_sim);

        ScanCommand = new RelayCommand(async () => await ScanAsync(), () => !IsBusy && UseBle);
        ConnectCommand = new RelayCommand(async () => await ConnectAsync(), () => !IsBusy && UseBle && SelectedDevice is not null);
        StartStopCommand = new RelayCommand(async () => await ToggleRunAsync(), () => !IsBusy);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<BleDiscoveredDevice> Devices { get; } = [];
    public ICommand ScanCommand { get; }
    public ICommand ConnectCommand { get; }
    public ICommand StartStopCommand { get; }
    public IReadOnlyList<double> SimRateOptions { get; } = [20, 100, 250, 500];

    public bool UseBle
    {
        get => _useBle;
        set
        {
            if (_useBle == value) return;
            _useBle = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(UseSim));
            OnPropertyChanged(nameof(ModeLabel));
            OnPropertyChanged(nameof(ActiveSourceName));
            _ = SwitchModeAsync();
        }
    }

    public bool UseSim { get => !_useBle; set => UseBle = !value; }
    public string ModeLabel => UseBle ? "真实设备 BLE" : "仿真数据";
    public string ActiveSourceName => _active.DisplayName;

    public string Status { get => _status; private set => SetField(ref _status, value); }
    public float Roll { get => _roll; private set => SetField(ref _roll, value); }
    public float Pitch { get => _pitch; private set => SetField(ref _pitch, value); }
    public float Yaw { get => _yaw; private set => SetField(ref _yaw, value); }

    /// <summary>Samples that crossed the driver boundary per second, whether or not they reached the screen.</summary>
    public double IngestHz { get => _ingestHz; private set => SetField(ref _ingestHz, value); }
    public double RenderFps { get => _renderFps; private set => SetField(ref _renderFps, value); }
    public long Dropped { get => _dropped; private set => SetField(ref _dropped, value); }

    /// <summary>Bumped when the stream is restarted so the render side can clear its own buffers.</summary>
    public ulong StreamGeneration => _generation;

    public double SimRateHz
    {
        get => _sim.RateHz;
        set
        {
            _sim.RateHz = value;
            OnPropertyChanged();
        }
    }

    public bool IsRunning => _active.State == ImuSourceState.Streaming;
    public string RunButtonText => IsRunning ? "停止" : "开始";

    public bool IsBusy
    {
        get => _isBusy;
        private set { if (SetField(ref _isBusy, value)) CommandManager.InvalidateRequerySuggested(); }
    }

    public BleDiscoveredDevice? SelectedDevice
    {
        get => _selectedDevice;
        set { if (SetField(ref _selectedDevice, value)) CommandManager.InvalidateRequerySuggested(); }
    }

    /// <summary>Consumer thread only: moves unread samples into <paramref name="destination"/>.</summary>
    public int DrainInto(Span<ImuSample> destination) => _ring.ReadNewestInto(destination);

    public async Task ToggleRunAsync()
    {
        if (IsRunning)
        {
            await _active.StopAsync().ConfigureAwait(true);
        }
        else
        {
            await _active.StartAsync().ConfigureAwait(true);
            // Clear on start, not on stop: the last frame is the one worth reading after a run ends.
            ResetStream();
        }

        Status = _active.StatusMessage ?? string.Empty;
        RaiseRunState();
    }

    /// <summary>
    /// Called once per render tick on the UI thread. Readouts and statistics are only ever updated here,
    /// so their cost is per frame rather than per sample.
    /// </summary>
    public void ReportFrame(ImuSample? newest, bool rendered)
    {
        if (newest is { } sample)
        {
            Roll = sample.Attitude.Roll;
            Pitch = sample.Attitude.Pitch;
            Yaw = sample.Attitude.Yaw;
        }

        if (rendered) _windowRenders++;

        long now = ImuSample.Now;
        if (_windowStartTicks == 0)
        {
            _windowIngestBase = Interlocked.Read(ref _ingestTotal);
            _windowStartTicks = now;
            return;
        }

        double elapsed = ImuSample.SecondsSince(_windowStartTicks, now);
        if (elapsed < StatsWindowSeconds) return;

        long ingest = Interlocked.Read(ref _ingestTotal);
        IngestHz = Math.Round((ingest - _windowIngestBase) / elapsed, 1);
        RenderFps = Math.Round(_windowRenders / elapsed, 1);
        Dropped = _ring.DroppedCount;

        _windowIngestBase = ingest;
        _windowRenders = 0;
        _windowStartTicks = now;
    }

    public async ValueTask DisposeAsync()
    {
        await _ble.DisposeAsync().ConfigureAwait(false);
        await _sim.DisposeAsync().ConfigureAwait(false);
    }

    private void ResetStream()
    {
        _ring.Clear();
        _ring.ResetDropped();
        Interlocked.Exchange(ref _ingestTotal, 0);
        _windowIngestBase = 0;
        _windowRenders = 0;
        _windowStartTicks = 0;
        _generation++;

        Roll = 0;
        Pitch = 0;
        Yaw = 0;
        IngestHz = 0;
        RenderFps = 0;
        Dropped = 0;
    }

    private async Task SwitchModeAsync()
    {
        IsBusy = true;
        try
        {
            await _active.StopAsync().ConfigureAwait(true);
            _active = UseBle ? _ble : _sim;
            Status = _active.StatusMessage ?? ModeLabel;
            ResetStream();
            RaiseRunState();
            OnPropertyChanged(nameof(ActiveSourceName));
        }
        finally { IsBusy = false; }
    }

    private async Task ScanAsync()
    {
        IsBusy = true;
        try
        {
            Status = "扫描中…";
            await _ble.ScanAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(true);
            Status = _ble.StatusMessage ?? "扫描结束";
        }
        finally { IsBusy = false; }
    }

    private async Task ConnectAsync()
    {
        if (SelectedDevice is null) return;
        IsBusy = true;
        try
        {
            Status = "连接中…";
            await _ble.ConnectAsync(SelectedDevice.Id).ConfigureAwait(true);
            Status = _ble.StatusMessage ?? string.Empty;
            RaiseRunState();
        }
        finally { IsBusy = false; }
    }

    private void Hook(IImuSource source)
    {
        source.SampleReceived += OnSample;
        source.StateChanged += (_, _) => { Status = source.StatusMessage ?? string.Empty; RaiseRunState(); };
    }

    private void OnSample(object? sender, ImuSample sample)
    {
        Interlocked.Increment(ref _ingestTotal);
        _ring.TryWrite(sample);
    }

    private void RefreshDevices()
    {
        System.Windows.Application.Current?.Dispatcher.Invoke(() =>
        {
            Devices.Clear();
            foreach (var d in _ble.DiscoveredDevices) Devices.Add(d);
            CommandManager.InvalidateRequerySuggested();
        });
    }

    private void RaiseRunState()
    {
        OnPropertyChanged(nameof(IsRunning));
        OnPropertyChanged(nameof(RunButtonText));
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }

    private sealed class RelayCommand : ICommand
    {
        private readonly Func<Task> _execute;
        private readonly Func<bool> _canExecute;
        public RelayCommand(Func<Task> execute, Func<bool> canExecute) { _execute = execute; _canExecute = canExecute; }
        public event EventHandler? CanExecuteChanged
        {
            add => CommandManager.RequerySuggested += value;
            remove => CommandManager.RequerySuggested -= value;
        }
        public bool CanExecute(object? parameter) => _canExecute();
        public async void Execute(object? parameter) => await _execute();
    }
}
