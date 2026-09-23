using System.Windows;
using System.Windows.Threading;
using NimBleImuHost.Ble;
using NimBleImuHost.ViewModels;

namespace NimBleImuHost;

/// <summary>
/// Render side of the pipeline. A timer tick drains the acquisition ring into preallocated arrays and
/// repaints in place, so display cost is per frame and never per sample.
/// </summary>
public partial class MainWindow : Window
{
    // Sliding window held by the chart: ~50 s at 20 Hz, 2 s at 500 Hz. Bounded because a scope that
    // keeps every point is a scope that redraws a million points per frame.
    private const int DisplayPoints = 1200;
    private const int DrainChunk = 2048;

    // DispatcherTimer quantises to the ~15.6 ms system timer, and asking for exactly one quantum
    // lands on every second one (~40 fps). Asking for half gets the 60 Hz the screen can show.
    private static readonly TimeSpan TickInterval = TimeSpan.FromMilliseconds(8);

    private readonly MainViewModel _vm;
    private readonly DispatcherTimer _timer;
    private readonly ImuSample[] _scratch = new ImuSample[DrainChunk];

    private readonly double[] _xs = new double[DisplayPoints];
    private readonly double[] _rolls = new double[DisplayPoints];
    private readonly double[] _pitches = new double[DisplayPoints];
    private readonly double[] _yaws = new double[DisplayPoints];

    private int _pointCount;
    private double _lastX;
    private long _prevTicks;
    private ulong _generation;
    private ScottPlot.IYAxis? _yawAxis;
    private ScottPlot.Plottables.SignalXY? _rollPlot;
    private ScottPlot.Plottables.SignalXY? _pitchPlot;
    private ScottPlot.Plottables.SignalXY? _yawPlot;

    public MainWindow()
    {
        InitializeComponent();
        _vm = new MainViewModel();
        DataContext = _vm;

        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TickInterval };
        _timer.Tick += OnRenderTick;

        Loaded += OnLoaded;
        Closed += async (_, _) =>
        {
            _timer.Stop();
            await _vm.DisposeAsync();
        };
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        var plt = Plot.Plot;
        // ScottPlot renders text through Skia, which has no CJK fallback here — keep plot chrome ASCII.
        plt.Title("roll / pitch (left axis) - yaw (right axis), deg");
        plt.XLabel("elapsed (s)");
        plt.Legend.IsVisible = true;
        plt.Legend.Alignment = ScottPlot.Alignment.UpperLeft;

        // yaw spans ±180°; on a shared axis it would flatten roll/pitch to a straight line.
        _yawAxis = plt.Axes.AddRightAxis();

        _timer.Start();
        Plot.Refresh();
    }

    private void OnRenderTick(object? sender, EventArgs e)
    {
        if (_vm.StreamGeneration != _generation) RestartWindow();

        int taken = _vm.DrainInto(_scratch);
        for (int i = 0; i < taken; i++)
            Append(_scratch[i]);

        bool rendered = false;
        if (taken > 0 && _pointCount >= 2)
        {
            EnsureSeries();
            SetVisibleRange();
            Plot.Plot.Axes.AutoScale();
            Plot.Refresh();
            rendered = true;
        }

        var newest = taken > 0 ? _scratch[taken - 1] : (ImuSample?)null;
        if (newest is { } sample)
            AttitudeView.SetAttitude(sample.Attitude);

        _vm.ReportFrame(newest, rendered);
    }

    private void Append(in ImuSample sample)
    {
        // Host-integration §4.2: notify pacing is uneven, so x accumulates real arrival intervals.
        double x = _pointCount == 0 ? 0 : _lastX + ImuSample.SecondsSince(_prevTicks, sample.MonoTicks);
        _prevTicks = sample.MonoTicks;

        // SignalXY needs strictly ascending X; two samples can share a Stopwatch tick.
        if (_pointCount > 0 && x <= _lastX) x = double.BitIncrement(_lastX);
        _lastX = x;

        if (_pointCount == DisplayPoints)
        {
            // Amortised: drop a quarter of the window at a time instead of one point per sample.
            const int keep = DisplayPoints - DisplayPoints / 4;
            Array.Copy(_xs, DisplayPoints - keep, _xs, 0, keep);
            Array.Copy(_rolls, DisplayPoints - keep, _rolls, 0, keep);
            Array.Copy(_pitches, DisplayPoints - keep, _pitches, 0, keep);
            Array.Copy(_yaws, DisplayPoints - keep, _yaws, 0, keep);
            _pointCount = keep;
        }

        _xs[_pointCount] = x;
        _rolls[_pointCount] = sample.Attitude.Roll;
        _pitches[_pointCount] = sample.Attitude.Pitch;
        _yaws[_pointCount] = sample.Attitude.Yaw;
        _pointCount++;
    }

    private void RestartWindow()
    {
        _generation = _vm.StreamGeneration;
        _pointCount = 0;
        _lastX = 0;
        _prevTicks = 0;
        _rollPlot = _pitchPlot = _yawPlot = null;
        Plot.Plot.Clear();
        Plot.Refresh();
    }

    private void EnsureSeries()
    {
        var plt = Plot.Plot;
        _rollPlot ??= AddSeries(plt, _rolls, "roll", "#FF6347", yAxis: null);
        _pitchPlot ??= AddSeries(plt, _pitches, "pitch", "#7CFC00", yAxis: null);
        _yawPlot ??= AddSeries(plt, _yaws, "yaw", "#4FC3F7", _yawAxis);
    }

    private ScottPlot.Plottables.SignalXY AddSeries(ScottPlot.Plot plt, double[] ys, string label, string hex, ScottPlot.IYAxis? yAxis)
    {
        var series = plt.Add.SignalXY(_xs, ys);
        series.LegendText = label;
        series.LineWidth = 1.4f;
        series.MarkerSize = 0;
        series.Color = ScottPlot.Color.FromHex(hex);
        if (yAxis is not null)
            series.Axes.YAxis = yAxis;
        return series;
    }

    private void SetVisibleRange()
    {
        // The arrays are fixed size, so the plottable would happily draw the stale tail beyond the
        // window; the index pair is what keeps a partially filled chart honest.
        int last = _pointCount - 1;
        ClipTo(_rollPlot, last);
        ClipTo(_pitchPlot, last);
        ClipTo(_yawPlot, last);
    }

    private static void ClipTo(ScottPlot.Plottables.SignalXY? series, int last)
    {
        if (series is null) return;
        series.Data.MinimumIndex = 0;
        series.Data.MaximumIndex = last;
    }
}
