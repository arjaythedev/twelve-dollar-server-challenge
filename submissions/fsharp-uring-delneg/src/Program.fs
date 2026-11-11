module Program

#nowarn "9"

open System
open System.IO
open System.Net
open System.Runtime.InteropServices

let private env name fallback =
    match Environment.GetEnvironmentVariable name with
    | null
    | "" -> fallback
    | v -> v

/// Locks the code and read-only data of the runtime, SQLite and the server's own image in RAM, up to RLIMIT_MEMLOCK.
/// With no swap, reclaim at the memory wall takes file-backed pages first, and the loop would stall on major faults
/// reading its own code back from disk. Executable segments first, the files the loop runs most first. Best effort.
let private lockCode () =
    try
        let maps =
            File.ReadAllLines "/proc/self/maps"
            |> Array.choose (fun l ->
                let f = l.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                if f.Length < 6 || not (f.[5].StartsWith "/") || f.[1].[0] <> 'r' || f.[1].[1] = 'w' then None
                else
                    let r = f.[0].Split '-'
                    Some(Convert.ToInt64(r.[0], 16), Convert.ToInt64(r.[1], 16), f.[1].[2] = 'x', Path.GetFileName f.[5]))
        let rank (name: string) =
            if name.StartsWith "libsqlite3td" then 0
            elif name.StartsWith "libcoreclr" || name.StartsWith "libclrjit" then 1
            elif name = "Server.dll" || name.StartsWith "System.Private.CoreLib" then 2
            elif name.StartsWith "libc.so" || name.StartsWith "libSystem.Native" then 3
            else 4
        let mutable locked = 0L
        for (a, b, _, _) in maps |> Array.sortBy (fun (_, _, x, n) -> (if x then 0 else 1), rank n) do
            if Native.mlock (nativeint a, unativeint (b - a)) = 0 then locked <- locked + (b - a)
        eprintfn "locked %d KB of code and read-only data" (locked >>> 10)
    with e -> eprintfn "lock code: %s" e.Message

[<EntryPoint>]
let main _ =
    let path = env "SQLITE_PATH" ""
    if path = "" then failwith "SQLITE_PATH is required"
    Jwt.init (env "JWT_SECRET" "twelve-dollar-challenge")
    let host = env "HOST" "0.0.0.0"
    let port = int (env "PORT" "80")
    let ip =
        match IPAddress.TryParse host with
        | true, a -> a
        | _ -> Dns.GetHostAddresses(host) |> Array.find (fun a -> a.AddressFamily = Sockets.AddressFamily.InterNetwork)
    // Accepted sockets live in io_uring fixed-file tables, not the descriptor table. Each table may hold up to
    // RLIMIT_NOFILE entries, so several rings lift the connection ceiling past the 65,535-descriptor limit.
    let rl = Array.zeroCreate<uint64> 2
    Native.getrlimit (7, rl) |> ignore
    rl.[0] <- rl.[1]
    Native.setrlimit (7, rl) |> ignore
    Native.getrlimit (7, rl) |> ignore
    let slots = int (min 65535UL rl.[0])
    let rings = max 1 (min 16 (Ring.envInt "RINGS" 8))
    let started = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
    let bt = Db.openDb path (Ring.envInt "CACHE_KB" 256000) (env "WARM" "1" = "1")
    let ck = if env "CKPT_THREAD" "1" = "1" then Db.Checkpointer(path, Ring.envInt "CKNICE" 19) else Unchecked.defaultof<_>
    let fds = Server.listen ip port (max 1 (Ring.envInt "LISTENERS" 8)) (Ring.envInt "DEFACC" 1)
    if env "LOCK" "1" = "1" then lockCode ()
    eprintfn "fsharp-uring on %O:%d: 1 loop thread, %d rings x %d slots, %d listeners, B-tree reads %s, SQLite %s"
        ip port rings slots fds.Length (if bt then "on" else "off") (Marshal.PtrToStringUTF8(Sqlite.sqlite3_libversion ()))
    Server.Worker(fds, slots, rings, ck, started).Run()
    0
