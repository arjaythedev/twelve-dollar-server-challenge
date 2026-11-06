package main

import (
	"sync/atomic"
	"time"
)

type writerMetrics struct {
	batches, writes, batchMax                                                               atomic.Uint64
	commits, commitNS, commitMax                                                            atomic.Uint64
	commitBuckets                                                                           [7]atomic.Uint64
	checkpoints, checkpointNS, checkpointMax, checkpointBusy, checkpointFrames, walMax      atomic.Uint64
	checkpointLogFrames, checkpointDoneFrames, walRestarts                                  atomic.Uint64
	restartCount, restartBusy, restartMax                                                   atomic.Uint64
	queueWaitCount, queueWaitNS, queueWaitMax                                               atomic.Uint64
	queueMax, rejected, loopMax                                                             atomic.Uint64
	writeErrors, writeErrorsBegin, writeErrorsStatement, writeErrorsCommit, writeErrorsBusy atomic.Uint64
	beginBusyRetries, beginBusyTimeouts                                                     atomic.Uint64
}

func maximum(a *atomic.Uint64, n uint64) {
	for old := a.Load(); n > old; old = a.Load() {
		if a.CompareAndSwap(old, n) {
			return
		}
	}
}
func (m *writerMetrics) commit(d time.Duration) {
	n := uint64(d)
	m.commits.Add(1)
	m.commitNS.Add(n)
	maximum(&m.commitMax, n)
	limits := [...]time.Duration{time.Millisecond, 10 * time.Millisecond, 100 * time.Millisecond, 500 * time.Millisecond, time.Second, 5 * time.Second}
	i := 0
	for i < len(limits) && d > limits[i] {
		i++
	}
	m.commitBuckets[i].Add(1)
}
func (m *writerMetrics) queueWait(d time.Duration) {
	n := uint64(d)
	m.queueWaitCount.Add(1)
	m.queueWaitNS.Add(n)
	maximum(&m.queueWaitMax, n)
}
func (m *writerMetrics) snapshot() map[string]any {
	mean := func(total, count uint64) float64 {
		if count == 0 {
			return 0
		}
		return float64(total) / float64(count)
	}
	buckets := make([]uint64, len(m.commitBuckets))
	for i := range buckets {
		buckets[i] = m.commitBuckets[i].Load()
	}
	return map[string]any{
		"batches_committed": m.batches.Load(), "writes_committed": m.writes.Load(), "writes_per_batch_mean": mean(m.writes.Load(), m.batches.Load()), "writes_per_batch_max": m.batchMax.Load(),
		"commit_count": m.commits.Load(), "commit_mean_ms": mean(m.commitNS.Load(), m.commits.Load()) / 1e6, "commit_max_ms": float64(m.commitMax.Load()) / 1e6,
		"commit_duration_buckets": buckets, "commit_bucket_upper_ms": []int{1, 10, 100, 500, 1000, 5000},
		"checkpoint_count": m.checkpoints.Load(), "checkpoint_mean_ms": mean(m.checkpointNS.Load(), m.checkpoints.Load()) / 1e6, "checkpoint_max_ms": float64(m.checkpointMax.Load()) / 1e6, "checkpoint_busy": m.checkpointBusy.Load(),
		"checkpoint_frames": m.checkpointFrames.Load(), "wal_size_max_bytes": m.walMax.Load(),
		"checkpoint_last_log_frames": m.checkpointLogFrames.Load(), "checkpoint_last_done_frames": m.checkpointDoneFrames.Load(), "wal_restarts_observed": m.walRestarts.Load(),
		"restart_count": m.restartCount.Load(), "restart_busy": m.restartBusy.Load(), "restart_max_ms": float64(m.restartMax.Load()) / 1e6,
		"queue_wait_mean_ms": mean(m.queueWaitNS.Load(), m.queueWaitCount.Load()) / 1e6, "queue_wait_max_ms": float64(m.queueWaitMax.Load()) / 1e6,
		"queue_depth_max": m.queueMax.Load(), "writes_rejected": m.rejected.Load(), "loop_iteration_max_ms": float64(m.loopMax.Load()) / 1e6,
		"write_errors": m.writeErrors.Load(), "write_errors_begin": m.writeErrorsBegin.Load(), "write_errors_statement": m.writeErrorsStatement.Load(), "write_errors_commit": m.writeErrorsCommit.Load(), "write_errors_busy": m.writeErrorsBusy.Load(),
		"begin_busy_retries": m.beginBusyRetries.Load(), "begin_busy_timeouts": m.beginBusyTimeouts.Load(),
	}
}
