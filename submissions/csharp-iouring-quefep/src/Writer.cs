using static Feed.Native;

namespace Feed;

/// <summary>One write request (new post or like), validated and authenticated by a ring worker, run by the <see cref="Writer"/>.</summary>
internal sealed class WriteJob {
    public const byte KindCreate = 1;
    public const byte KindLike = 2;

    public byte Kind;
    public long UserId, PostId;
    public readonly byte[] Body = new byte[1536];   // trimmed body as UTF-8: at most 500 UTF-16 chars, so at most 1,500 bytes
    public int BodyLength;
    public readonly byte[] Username = new byte[1024];
    public int UsernameLength;

    // Where the response goes.
    public Worker Owner = null!;
    public int Slot;
    public uint Gen;
    public bool Close;

    // Filled in by the writer.
    public int Status;
    public readonly Out Response = new(4096);
}

/// <summary>
/// The only thread that writes to SQLite, using its own connection. Ring workers queue checked
/// writes here. The writer runs the queued jobs in one IMMEDIATE transaction, commits once, and
/// only then hands each response back to its worker. Checkpoints use a third connection
/// (<see cref="Checkpointer"/>). If SQLite has to wait on dirty pages, that wait is confined to
/// writes; the read path on the ring threads does not wait for this thread.
/// </summary>
internal sealed class Writer {
    private const int MaxBatch = 1024;

    private readonly Db _db;
    private readonly Checkpointer _checkpointer;
    private readonly Lock _gate = new();
    private readonly AutoResetEvent _wake = new(false);
    private List<WriteJob> _queue = new(MaxBatch);
    private List<WriteJob> _work = new(MaxBatch);
    private readonly Stack<WriteJob> _pool = new();

    public Writer(Db db, Checkpointer checkpointer) {
        _db = db;
        _checkpointer = checkpointer;
        Thread t = new(Run, 1 << 20) { IsBackground = true, Name = "writer" };
        t.Start();
    }

    /// <summary>Returns a reusable write job; the caller fills it in and submits it.</summary>
    public WriteJob Rent() {
        lock (_gate) {
            return _pool.Count > 0 ? _pool.Pop() : new WriteJob();
        }
    }

    /// <summary>Returns a finished job to the pool.</summary>
    public void Return(WriteJob job) {
        lock (_gate) {
            _pool.Push(job);
        }
    }

    /// <summary>Queues a filled-in job.</summary>
    public void Submit(WriteJob job) {
        bool wake;
        lock (_gate) {
            wake = _queue.Count == 0;
            _queue.Add(job);
        }
        if (wake) {
            _wake.Set();
        }
    }

    private void Run() {
        try {
            while (true) {
                _wake.WaitOne(1000);
                _wake.WaitOne(1); // coalesce a burst into one commit; the ring's own wait is longer
                while (true) {
                    lock (_gate) {
                        (_queue, _work) = (_work, _queue);
                    }
                    if (_work.Count == 0) {
                        break;
                    }

                    for (int start = 0; start < _work.Count; start += MaxBatch) {
                        RunBatch(start, Math.Min(_work.Count, start + MaxBatch));
                    }
                    _work.Clear();
                    _db.RequestCheckpoint(_checkpointer);
                }
            }
        } catch (Exception ex) {
            Log($"fatal: writer: {ex}");
            Environment.Exit(1);
        }
    }

    /// <summary>Runs a group of jobs in one transaction and hands back their committed responses.</summary>
    private void RunBatch(int start, int end) {
        bool ok = _db.BeginImmediate();
        for (int i = start; i < end && ok; i++) {
            WriteJob job = _work[i];
            job.Response.Reset();
            job.Status = Execute(job);
            if (!_db.InTransaction) {
                ok = false; // SQLite rolled the transaction back: nothing in it is committed
            }
        }
        ok = ok && _db.Commit();
        if (!ok && _db.InTransaction) {
            _db.Rollback();
        }

        for (int i = start; i < end; i++) {
            WriteJob job = _work[i];
            if (!ok) {
                job.Status = 500;
                job.Response.Reset();
                job.Response.Bytes(App.InternalError);
            }
            job.Owner.Complete(job);
        }
    }

    /// <summary>Runs one write and writes its JSON body. Returns the HTTP status.</summary>
    private int Execute(WriteJob job) {
        Out o = job.Response;
        try {
            if (job.Kind == WriteJob.KindCreate) {
                if (_db.CreatePost(job.UserId, job.Body.AsSpan(0, job.BodyLength), job.Username.AsSpan(0, job.UsernameLength), o)) {
                    return 201;
                }
            } else {
                LikeOutcome outcome = _db.Like(job.UserId, job.PostId);
                if (outcome == LikeOutcome.PostMissing) {
                    o.Bytes("{\"error\":\"post not found\"}"u8);
                    return 404;
                }
                if (outcome != LikeOutcome.Failed) {
                    o.Bytes(outcome == LikeOutcome.Liked
                        ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"u8
                        : "{\"liked\":true,\"already_liked\":true,\"post_id\":"u8);
                    o.Int(job.PostId);
                    o.Byte((byte)'}');
                    return outcome == LikeOutcome.Liked ? 201 : 200;
                }
            }
        } catch (Exception ex) {
            Log(ex.ToString());
        }
        o.Reset();
        o.Bytes(App.InternalError);
        return 500;
    }
}
