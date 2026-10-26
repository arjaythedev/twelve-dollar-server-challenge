using static Feed.Native;

namespace Feed;

/// <summary>
/// Watches available memory so the server can stop accepting instead of being OOM-killed. The
/// 2 GB budget is shared with kernel socket memory, which grows with the connection count.
/// Usable = MemAvailable - SReclaimable - CmaFree, following the C io_uring entry (PR #112).
/// Below 192 MB the rings stop accepting; above 288 MB they accept again.
///
/// Established connections are left alone on purpose. In the local 1 vCPU / 2 GB tests, forcing
/// reconnects under memory pressure produced a reconnect storm and made things worse.
/// </summary>
internal static unsafe class MemGuard {
    private const long StopKb = 192 * 1024;
    private const long ResumeKb = 288 * 1024;

    private static int _fd = -2;
    private static readonly byte[] Buf = new byte[4096];

    /// <summary>0 = normal, 1 = don't accept new connections.</summary>
    public static volatile int Level;

    /// <summary>Re-reads /proc/meminfo and updates Level. Called by one worker a few times a second.</summary>
    public static void Update() {
        if (_fd == -2) {
            fixed (byte* path = "/proc/meminfo\0"u8) {
                _fd = open(path, O_RDONLY | O_CLOEXEC, 0);
            }
        }
        if (_fd < 0) {
            return;
        }

        int n;
        fixed (byte* b = Buf) {
            n = (int)pread(_fd, b, Buf.Length, 0);
        }
        if (n <= 0) {
            return;
        }

        ReadOnlySpan<byte> text = Buf.AsSpan(0, n);
        long avail = Field(text, "MemAvailable:"u8);
        if (avail < 0) {
            return;
        }

        long usable = avail - Math.Max(0, Field(text, "SReclaimable:"u8)) - Math.Max(0, Field(text, "CmaFree:"u8));
        int level = Level;
        if (usable < StopKb) {
            level = 1;
        } else if (usable > ResumeKb) {
            level = 0;
        }
        if (level != Level) {
            Log($"memory: {usable / 1024} MB usable, level {Level} -> {level}");
            Level = level;
        }
    }

    /// <summary>The kB value of a /proc/meminfo field, or -1 if it is missing.</summary>
    private static long Field(ReadOnlySpan<byte> text, ReadOnlySpan<byte> name) {
        int i = text.IndexOf(name);
        if (i < 0) {
            return -1;
        }

        long value = 0;
        bool digits = false;
        for (i += name.Length; i < text.Length && text[i] != (byte)'\n'; i++) {
            byte c = text[i];
            if (c is >= (byte)'0' and <= (byte)'9') {
                value = (value * 10) + (c - '0');
                digits = true;
            } else if (digits) {
                break;
            }
        }
        return digits ? value : -1;
    }
}
