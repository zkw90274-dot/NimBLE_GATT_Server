using NimBleImuHost.Protocol;

namespace NimBleImuHost.Ble;

/// <summary>
/// Synthetic attitude stream for demos and for load-testing the host without hardware.
/// Produces smooth roll/pitch oscillation and a slowly drifting yaw at a configurable rate.
/// </summary>
public sealed class SimulatedImuSource : IImuSource
{
    private readonly object _gate = new();
    private CancellationTokenSource? _cts;
    private Task? _loop;
    private ImuSourceState _state = ImuSourceState.Idle;
    private string? _status = "仿真模式就绪";
    private double _rateHz = 20;

    public string DisplayName => "Simulation";

    /// <summary>Target sample rate. Applies immediately to a running stream.</summary>
    public double RateHz
    {
        get { lock (_gate) return _rateHz; }
        set
        {
            double clamped = Math.Clamp(value, 1, 2000);
            lock (_gate) _rateHz = clamped;
        }
    }

    public ImuSourceState State
    {
        get { lock (_gate) return _state; }
    }

    public string? StatusMessage
    {
        get { lock (_gate) return _status; }
    }

    public event EventHandler<ImuSample>? SampleReceived;
    public event EventHandler<ImuSourceState>? StateChanged;

    public Task StartAsync(CancellationToken cancellationToken = default)
    {
        lock (_gate)
        {
            if (_loop is { IsCompleted: false })
                return Task.CompletedTask;

            _cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            var token = _cts.Token;
            SetState(ImuSourceState.Streaming, $"仿真数据流运行中（{_rateHz:0} Hz）");
            _loop = Task.Run(() => RunAsync(token), CancellationToken.None);
        }

        return Task.CompletedTask;
    }

    public async Task StopAsync()
    {
        Task? loop;
        CancellationTokenSource? cts;
        lock (_gate)
        {
            cts = _cts;
            loop = _loop;
            _cts = null;
            _loop = null;
        }

        if (cts is not null)
        {
            await cts.CancelAsync().ConfigureAwait(false);
            cts.Dispose();
        }

        if (loop is not null)
        {
            try { await loop.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
        }

        SetState(ImuSourceState.Idle, "仿真已停止");
    }

    public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);

    private async Task RunAsync(CancellationToken token)
    {
        long start = ImuSample.Now;
        long next = start;

        while (!token.IsCancellationRequested)
        {
            double rate = RateHz;
            long periodTicks = StopwatchTicksPerSecond() / (long)rate;

            double t = (ImuSample.Now - start) / (double)StopwatchTicksPerSecond();

            // Gentle motion demo: roll/pitch sinusoid, yaw ramp with small noise.
            float roll = (float)(12.0 * Math.Sin(t * 0.9));
            float pitch = (float)(8.0 * Math.Sin(t * 0.55 + 0.4));
            float yaw = (float)(((t * 6.0) % 360.0 > 180 ? (t * 6.0) % 360.0 - 360.0 : (t * 6.0) % 360.0)
                                + 0.05 * Math.Sin(t * 3.1));

            SampleReceived?.Invoke(this, new ImuSample(new AttitudePacket(roll, pitch, yaw), ImuSample.Now));

            next += periodTicks;
            long remaining = next - ImuSample.Now;
            if (remaining <= 0)
            {
                next = ImuSample.Now;
                continue;
            }

            // Task.Delay floors at one system tick (~15.6 ms) on Windows, so it can only be used when there
            // is more than a tick of slack; below that the deadline is met by spinning. A 100 Hz+ stream
            // therefore burns a core in the generator — the honest price of pacing without a multimedia timer.
            double remainingSeconds = remaining / (double)StopwatchTicksPerSecond();
            if (remainingSeconds > 0.020)
            {
                try
                {
                    await Task.Delay(TimeSpan.FromSeconds(remainingSeconds - 0.015), token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) { break; }
            }

            while (ImuSample.Now < next && !token.IsCancellationRequested)
                Thread.SpinWait(20);
        }
    }

    private static long StopwatchTicksPerSecond() => System.Diagnostics.Stopwatch.Frequency;

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
