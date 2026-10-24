using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

internal static unsafe class Program {
    private const int MaxSlotsPerRing = 65535;
    private const int TargetSlots = 5 * MaxSlotsPerRing; // connection capacity aimed for, across rings
    private const int MaxRings = 8;

    private static int Main() {
        string? dbPath = Environment.GetEnvironmentVariable("SQLITE_PATH");
        string? secret = Environment.GetEnvironmentVariable("JWT_SECRET");
        if (string.IsNullOrEmpty(dbPath) || string.IsNullOrEmpty(secret)) {
            Log("SQLITE_PATH and JWT_SECRET are required");
            return 1;
        }
        string host = Environment.GetEnvironmentVariable("HOST") is { Length: > 0 } h ? h : "127.0.0.1";
        int port = int.TryParse(Environment.GetEnvironmentVariable("PORT"), out int p) ? p : 3000;

        // Direct descriptor tables are capped at RLIMIT_NOFILE each: raise the soft limit to the hard one.
        RLimit lim;
        getrlimit(RLIMIT_NOFILE, &lim);
        lim.Cur = lim.Max;
        setrlimit(RLIMIT_NOFILE, &lim);
        getrlimit(RLIMIT_NOFILE, &lim);
        int slots = (int)Math.Min(lim.Cur, MaxSlotsPerRing);
        int rings = Math.Clamp((TargetSlots + slots - 1) / slots, 1, MaxRings);

        Db reader = new(dbPath, writer: false);
        Writer writer = new(new Db(dbPath, writer: true), new Checkpointer(dbPath));
        App app = new(reader, new Api(reader, new Jwt(Encoding.UTF8.GetBytes(secret)), writer));
        int listen = Listen(host, port);
        MemGuard.Update();

        Log($"listening on {host}:{port} ({rings} io_uring rings x {slots} direct descriptors)");
        for (int i = 1; i < rings; i++) {
            Worker w = new(i, app, listen, slots);
            Thread t = new(() => Serve(w), 1 << 20) { IsBackground = true, Name = $"ring{i}" };
            t.Start();
        }
        Serve(new Worker(0, app, listen, slots));
        return 1;
    }

    private static void Serve(Worker w) {
        try {
            w.Run();
        } catch (Exception ex) {
            Log($"fatal: {ex}");
            Environment.Exit(1);
        }
    }

    /// <summary>A listening TCP socket. TCP_NODELAY set here is inherited by every accepted socket.</summary>
    private static int Listen(string host, int port) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            Fail("socket");
        }

        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(int));
        SockAddrIn addr = new() { Family = AF_INET, Port = (ushort)((port >> 8) | ((port & 0xFF) << 8)) };
        byte[] hostBytes = Encoding.ASCII.GetBytes((host == "localhost" ? "127.0.0.1" : host) + "\0");
        fixed (byte* hp = hostBytes) {
            if (inet_pton(AF_INET, hp, &addr.Addr) != 1) {
                Log($"HOST must be an IPv4 address: {host}");
                Environment.Exit(1);
            }
        }
        if (bind(fd, &addr, (uint)sizeof(SockAddrIn)) < 0) {
            Fail("bind");
        }

        if (listen(fd, 65535) < 0) {
            Fail("listen");
        }

        return fd;
    }

    private static void Fail(string what) {
        Log($"{what} failed: errno {Marshal.GetLastPInvokeError()}");
        Environment.Exit(1);
    }
}
