using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Threading.RateLimiting;

namespace FeedApi;

internal sealed class DatabaseMetrics : IDisposable
{
    private readonly Meter _meter = new("DevvGuru.FeedApi", "1.0.0");
    private readonly Histogram<double> _operation;
    private readonly Histogram<double> _commit;
    private readonly Histogram<double> _writeWait;
    private readonly Counter<long> _writeRejected;

    public DatabaseMetrics(ConcurrencyLimiter writes)
    {
        _operation = _meter.CreateHistogram<double>("feed.db.duration", "ms");
        _commit = _meter.CreateHistogram<double>("feed.db.commit", "ms");
        _writeWait = _meter.CreateHistogram<double>("feed.db.write_wait", "ms");
        _writeRejected = _meter.CreateCounter<long>("feed.db.write_rejected", "{request}");
        _meter.CreateObservableGauge("feed.db.write_queued", () => writes.GetStatistics()?.CurrentQueuedCount ?? 0, "{request}");
        _meter.CreateObservableGauge("feed.db.write_active", () => 1 - (writes.GetStatistics()?.CurrentAvailablePermits ?? 1), "{request}");
        _meter.CreateObservableGauge("feed.runtime.worker_threads", () => ThreadPool.ThreadCount, "{thread}");
        _meter.CreateObservableGauge("feed.runtime.pending_work", () => ThreadPool.PendingWorkItemCount, "{work_item}");
    }

    public Duration Measure(string operation) => new(_operation, operation);
    public Duration MeasureCommit(string operation) => new(_commit, operation);
    public void WriteWait(double milliseconds) => _writeWait.Record(milliseconds);
    public void WriteRejected(string reason) => _writeRejected.Add(1, new KeyValuePair<string, object?>("reason", reason));
    public void Dispose() => _meter.Dispose();

    internal readonly struct Duration(Histogram<double> histogram, string operation) : IDisposable
    {
        private readonly long _started = Stopwatch.GetTimestamp();
        public void Dispose() => histogram.Record(Stopwatch.GetElapsedTime(_started).TotalMilliseconds,
            new KeyValuePair<string, object?>("operation", operation));
    }
}
