using System.Diagnostics;
using NimBleImuHost.Ble;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Tests;

/// <summary>
/// The acquisition ring is the only thing between the driver callback thread and the render tick, so its
/// ordering, overflow and accounting behaviour are what the 100 fps claim rests on.
/// </summary>
public class AttitudeRingBufferTests
{
    private static ImuSample Sample(float yaw) => new(new AttitudePacket(1f, 2f, yaw), ImuSample.Now);

    [Fact]
    public void ReadReturnsWrittenOrder()
    {
        var ring = new AttitudeRingBuffer(16);

        for (int i = 0; i < 5; i++)
            Assert.True(ring.TryWrite(Sample(i)));

        ImuSample[] buffer = new ImuSample[5];
        Assert.Equal(5, ring.ReadNewestInto(buffer));
        Assert.Equal([0f, 1f, 2f, 3f, 4f], buffer.Select(s => s.Attitude.Yaw));
    }

    [Fact]
    public void EmptyRingReadsZero()
    {
        var ring = new AttitudeRingBuffer(16);
        Assert.Equal(0, ring.ReadNewestInto(new ImuSample[4]));
    }

    [Fact]
    public void WrapAroundKeepsOrder()
    {
        var ring = new AttitudeRingBuffer(4);
        ImuSample[] scratch = new ImuSample[4];

        for (int round = 0; round < 3; round++)
        {
            for (int i = 0; i < 3; i++)
                Assert.True(ring.TryWrite(Sample(round * 10 + i)));

            int taken = ring.ReadNewestInto(scratch);
            Assert.Equal(3, taken);
            Assert.Equal([round * 10f, round * 10 + 1f, round * 10 + 2f], scratch[..taken].Select(s => s.Attitude.Yaw));
        }
    }

    [Fact]
    public void Overflow_RefusesNewest_AndCountsItAsDropped()
    {
        var ring = new AttitudeRingBuffer(2);

        Assert.True(ring.TryWrite(Sample(0)));
        Assert.True(ring.TryWrite(Sample(1)));
        Assert.False(ring.TryWrite(Sample(2)));
        Assert.Equal(1, ring.DroppedCount);

        ImuSample[] buffer = new ImuSample[2];
        Assert.Equal(2, ring.ReadNewestInto(buffer));
        Assert.Equal([0f, 1f], buffer.Select(s => s.Attitude.Yaw));
    }

    [Fact]
    public void LaggingConsumer_TakesNewest_AndSkipsSuperseded()
    {
        var ring = new AttitudeRingBuffer(64);

        for (int i = 0; i < 20; i++)
            Assert.True(ring.TryWrite(Sample(i)));

        // A render tick that only has room for the last few points must not show stale data.
        ImuSample[] buffer = new ImuSample[4];
        Assert.Equal(4, ring.ReadNewestInto(buffer));
        Assert.Equal([16f, 17f, 18f, 19f], buffer.Select(s => s.Attitude.Yaw));
        Assert.Equal(0, ring.DroppedCount);
    }

    [Fact]
    public void ClearDiscardsUnread_AndNothingMoreIsRead()
    {
        var ring = new AttitudeRingBuffer(16);
        for (int i = 0; i < 6; i++) ring.TryWrite(Sample(i));

        ring.Clear();

        Assert.Equal(0, ring.ReadNewestInto(new ImuSample[8]));
        Assert.True(ring.TryWrite(Sample(7)));
        Assert.Equal(1, ring.ReadNewestInto(new ImuSample[8]));
    }

    [Fact]
    public void ResetDropped_ZeroesTheCounter()
    {
        var ring = new AttitudeRingBuffer(1);
        ring.TryWrite(Sample(0));
        ring.TryWrite(Sample(1));
        Assert.True(ring.DroppedCount > 0);

        ring.ResetDropped();
        Assert.Equal(0, ring.DroppedCount);
    }

    [Fact]
    public void NonPowerOfTwoCapacity_IsRejected()
    {
        Assert.Throws<ArgumentException>(() => new AttitudeRingBuffer(1000));
        Assert.Throws<ArgumentException>(() => new AttitudeRingBuffer(0));
    }

    [Fact]
    public async Task ProducerConsumer_ConserveSamples()
    {
        const int written = 500_000;
        var ring = new AttitudeRingBuffer(16_384);
        ImuSample[] buffer = new ImuSample[ring.Capacity];

        int read = 0;
        float lastSeen = float.MinValue;
        bool ordered = true;

        var producer = Task.Run(() =>
        {
            // One attempt per sample, exactly like the driver callback: a refused sample is gone.
            for (int i = 0; i < written; i++)
                ring.TryWrite(Sample(i));
        });

        while (true)
        {
            int taken = ring.ReadNewestInto(buffer);
            foreach (var sample in buffer[..taken])
            {
                if (sample.Attitude.Yaw <= lastSeen) ordered = false;
                lastSeen = sample.Attitude.Yaw;
            }
            read += taken;

            if (taken == 0 && producer.IsCompleted) break;
            if (taken == 0) await Task.Yield();
        }

        await producer;

        // The ring refuses rather than overwriting, so every sample is either displayed or counted.
        Assert.True(ordered);
        Assert.Equal(written, read + ring.DroppedCount);
    }

    [Fact]
    public void SteadyState_CopyRate_Sustains100Hz()
    {
        var ring = new AttitudeRingBuffer(16_384);
        ImuSample[] buffer = new ImuSample[64];

        const int iterations = 1_000_000;
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < iterations; i++)
        {
            ring.TryWrite(Sample(i % 360));
            if (i % 8 == 7) ring.ReadNewestInto(buffer);
        }
        sw.Stop();

        double perSecond = iterations / sw.Elapsed.TotalSeconds;
        Assert.True(perSecond > 1_000_000,
            $"ring hot path only managed {perSecond:N0} samples/s; the UI thread would burn there");
    }
}
