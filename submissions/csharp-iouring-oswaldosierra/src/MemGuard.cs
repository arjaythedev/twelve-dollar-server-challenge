using static Feed.Native;

namespace Feed;

/// <summary>
/// Watches the box's usable memory so the server degrades instead of being OOM-killed: 2 GB is
/// shared with the kernel's socket buffers, and those grow with the number of connections.
/// Usable = MemAvailable − SReclaimable − CmaFree (as in the C io_uring entry, PR #112).
/// Level 1 (below 192 MB): stop accepting. Level 2 (below 96 MB): also answer with
/// Connection: close so clients release sockets. Back to 0 above 288 MB (hysteresis).
/// </summary>
internal static unsafe class MemGuard {
    private const long StopKb = 192 * 1024;
    private const long ShedKb = 96 * 1024;
    private const long ResumeKb = 288 * 1024;

    private static int _fd = -2;
    private static readonly byte[] Buf = new byte[4096];

    /// <summary>0 = normal, 1 = don't accept, 2 = don't accept and close connections after their response.</summary>
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
        if (usable < ShedKb) {
            level = 2;
        } else if (usable < StopKb) {
            level = Math.Max(level, 1);
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
