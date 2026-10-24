using System.Buffers;
using System.Buffers.Text;

namespace Feed;

/// <summary>
/// State shared by every ring worker: the SQLite connection, the API and the batch transaction.
/// A worker holds <see cref="Gate"/> for a whole batch: it opens the transaction lazily on the
/// batch's first request, runs every request, commits once, and only then queues the responses
/// (rule 6: a 201 is sent after its row is committed). With one vCPU the lock is uncontended in
/// practice; it only serializes the workers.
/// </summary>
internal sealed class App(Db db, Api api) {
    public readonly Api Api = api;

    /// <summary>Held for a whole batch, and for checkpoints.</summary>
    public readonly Lock Gate = new();

    public readonly Db Db = db;
    private bool _inBatchTransaction;
    private bool _batchBroken;   // the batch transaction failed or was rolled back: answer the rest with 500s

    private readonly byte[] _date = new byte[29]; // cached Date header value (RFC 1123, 29 bytes)
    private long _dateSecond = -1;

    /// <summary>Starts a batch. Caller holds Gate.</summary>
    public void BeginBatch() {
        _inBatchTransaction = false;
        _batchBroken = false;
    }

    /// <summary>
    /// Runs one request inside the batch transaction and writes its body into <paramref name="body"/>.
    /// Returns the HTTP status. Once the batch transaction has failed, requests are not run and get a 500.
    /// </summary>
    public int Handle(ReadOnlySpan<byte> method, ReadOnlySpan<byte> target, ReadOnlySpan<byte> auth, bool hasAuth,
        ReadOnlySpan<byte> requestBody, Out body) {
        if (!_batchBroken && !_inBatchTransaction) {
            if (Db.InTransaction) {
                Db.Rollback(); // never run a batch inside a transaction left over from a failure
            }

            _inBatchTransaction = Db.Begin();
            _batchBroken = !_inBatchTransaction;
        }

        body.Reset();
        int status = 500;
        if (!_batchBroken) {
            try {
                status = Api.Handle(method, target, auth, hasAuth, requestBody, body);
            } catch (Exception ex) {
                Native.Log(ex.ToString());
                status = 500;
            }
            // SQLite rolls the whole transaction back on some errors; anything after that would autocommit.
            if (!Db.InTransaction) {
                _batchBroken = true;
            }
        }
        if (status == 500) {
            body.Reset();
            body.Bytes(InternalError);
        }

        return status; // Api.Deferred: queued to the Writer, which answers later
    }

    /// <summary>Ends the batch: commits it. False if the batch failed, in which case every response in it must become a 500.</summary>
    public bool EndBatch() {
        if (_batchBroken) {
            if (Db.InTransaction) {
                Db.Rollback();
            }

            return false;
        }
        return !_inBatchTransaction || Db.Commit();
    }

    public static ReadOnlySpan<byte> InternalError => "{\"error\":\"internal server error\"}"u8;

    /// <summary>Writes a full response into <paramref name="o"/>. Statuses without a case below get the 500 status line.</summary>
    public void WriteResponse(Out o, int status, ReadOnlySpan<byte> body, bool close) {
        o.Bytes(status switch {
            200 => "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nDate: "u8,
            201 => "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nDate: "u8,
            400 => "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nDate: "u8,
            401 => "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nDate: "u8,
            404 => "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nDate: "u8,
            503 => "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nDate: "u8,
            _ => "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nDate: "u8,
        });
        o.Bytes(Date());
        o.Bytes("\r\nContent-Length: "u8);
        o.Int(body.Length);
        o.Bytes(close ? "\r\nConnection: close\r\n\r\n"u8 : "\r\n\r\n"u8);
        o.Bytes(body);
    }

    /// <summary>The Date header value, reformatted at most once per second.</summary>
    private ReadOnlySpan<byte> Date() {
        long second = Environment.TickCount64 / 1000;
        if (second != _dateSecond) {
            _dateSecond = second;
            Utf8Formatter.TryFormat(DateTime.UtcNow, _date, out _, new StandardFormat('R'));
        }
        return _date;
    }
}
