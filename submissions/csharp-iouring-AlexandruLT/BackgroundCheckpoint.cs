using System.Runtime.InteropServices;

namespace Feed;

// Checkpoints the WAL on its own connection and thread, so the poll thread never blocks on
// checkpoint I/O. It touches no request state or transport.
sealed class BackgroundCheckpoint : IDisposable
{
    const int Passive = 0, Restart = 2, Busy = 5, Locked = 6;
    readonly Db writer;
    readonly int pages;
    readonly nint handle;
    readonly Thread thread;
    readonly ManualResetEvent stop = new(false);
    long calls, lastSerial;
    public BackgroundCheckpoint(string path, Db writer, int pages)
    {
        if (pages < 1) throw new ArgumentOutOfRangeException(nameof(pages));
        this.writer = writer; this.pages = pages;
        int rc = Sqlite.sqlite3_open_v2(path, out handle, 2 | 0x8000, 0);
        if (rc != 0) throw new InvalidOperationException($"checkpoint open: {rc}");
        rc = Sqlite.sqlite3_exec(handle, "PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=0; PRAGMA wal_autocheckpoint=0;", 0, 0, 0);
        if (rc != 0) { Sqlite.sqlite3_close_v2(handle); throw new InvalidOperationException($"checkpoint setup: {rc}"); }
        thread = new Thread(Run) { IsBackground = true, Name = "sqlite checkpoint" };
        thread.Start();
    }
    void Run()
    {
        while (!stop.WaitOne(25))
        {
            long serial = writer.WalSerial;
            if (serial == lastSerial || writer.WalFrames < pages) continue;
            // PASSIVE normally lets the next writer rewind the WAL. RESTART is an occasional
            // fallback and never waits: busy_timeout is 0 on this connection.
            int mode = ++calls % 64 == 0 ? Restart : Passive;
            int rc = Sqlite.sqlite3_wal_checkpoint_v2(handle, "main", mode, out int frames, out int done);
            if (rc != 0 && rc != Busy && rc != Locked)
                Console.Error.WriteLine($"checkpoint SQLite {rc}: {Marshal.PtrToStringUTF8(Sqlite.sqlite3_errmsg(handle))}");
            // Retry incomplete work after 25 ms even if no later commit arrives.
            if (rc == 0 && frames >= 0 && done == frames) lastSerial = serial;
        }
    }
    public void Dispose()
    {
        stop.Set(); thread.Join();
        Sqlite.sqlite3_close_v2(handle); stop.Dispose();
    }
}
