using System.Threading;

namespace NimBleImuHost.Ble;

/// <summary>
/// Single-producer / single-consumer ring for the attitude stream: the BLE or simulator callback thread
/// writes, the UI render tick reads. Allocation-free on the hot path.
/// </summary>
/// <remarks>
/// Modelled on Serial Studio's acquisition pipeline: a bounded queue between the producer and the
/// consumer, and a <see cref="DroppedCount"/> that is the canary — if it climbs, the consumer has
/// stalled and the UI must be made cheaper, not the buffer bigger.
/// On overflow the <em>newest</em> sample is refused (strict SPSC: only the consumer may advance the
/// read cursor). When the consumer lags behind by more than it can display, the oldest unread samples
/// are skipped rather than counted as drops — they are superseded display data, not lost telemetry.
/// </remarks>
public sealed class AttitudeRingBuffer
{
    private readonly ImuSample[] _slots;
    private readonly int _mask;

    private int _head;   // write cursor: producer writes here, then publishes _head + 1
    private int _tail;   // read cursor: consumer-owned
    private long _dropped;

    public AttitudeRingBuffer(int capacity = 16384)
    {
        if (capacity <= 0 || (capacity & (capacity - 1)) != 0)
            throw new ArgumentException("capacity must be a positive power of two", nameof(capacity));

        _slots = new ImuSample[capacity];
        _mask = capacity - 1;
    }

    public int Capacity => _slots.Length;

    /// <summary>Refused writes — one per sample the UI never saw, provided the producer does not retry.</summary>
    public long DroppedCount => Interlocked.Read(ref _dropped);

    /// <summary>Producer thread only.</summary>
    /// <returns>false when the ring is full and the sample was refused.</returns>
    public bool TryWrite(in ImuSample sample)
    {
        int head = _head;
        if (head - Volatile.Read(ref _tail) == _slots.Length)
        {
            Interlocked.Increment(ref _dropped);
            return false;
        }

        _slots[head & _mask] = sample;
        Volatile.Write(ref _head, head + 1);
        return true;
    }

    /// <summary>
    /// Consumer thread only: takes the newest <paramref name="destination"/>.Length unread samples,
    /// skipping older ones. Returns how many were written.
    /// </summary>
    public int ReadNewestInto(Span<ImuSample> destination)
    {
        int head = Volatile.Read(ref _head);
        int tail = _tail;
        int unread = head - tail;

        if (unread <= 0)
            return 0;

        int skip = Math.Max(0, unread - destination.Length);
        int take = unread - skip;
        int start = tail + skip;

        for (int i = 0; i < take; i++)
            destination[i] = _slots[(start + i) & _mask];

        Volatile.Write(ref _tail, start + take);
        return take;
    }

    /// <summary>Consumer thread only: discards everything unread.</summary>
    public void Clear() => Volatile.Write(ref _tail, Volatile.Read(ref _head));

    public void ResetDropped() => Interlocked.Exchange(ref _dropped, 0);
}
