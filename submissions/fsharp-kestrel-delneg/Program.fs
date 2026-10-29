module Program

open System
open System.Net
open System.Text
open System.Threading
open System.Threading.Tasks
open Microsoft.AspNetCore.Builder
open Microsoft.AspNetCore.Connections
open Microsoft.AspNetCore.Hosting
open Microsoft.AspNetCore.Hosting.Server
open Microsoft.Extensions.DependencyInjection

let env name fallback =
    match Environment.GetEnvironmentVariable name with
    | null
    | "" -> fallback
    | v -> v

/// Kestrel needs an IHttpApplication to start, but the connection handler answers every request itself,
/// so Kestrel's HTTP layer never calls this.
type private Unused() =
    interface IHttpApplication<obj> with
        member _.CreateContext(_) = null
        member _.ProcessRequestAsync(_) = Task.CompletedTask
        member _.DisposeContext(_, _) = ()

[<EntryPoint>]
let main _ =
    // Socket completions run on .NET's socket thread, not a thread-pool worker: on one core that hand-off
    // is a context switch per request. Read when the first socket is created, so set it first.
    Environment.SetEnvironmentVariable("DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS", "1")
    Db.openDb (env "SQLITE_PATH" "feed.db")
    Auth.key <- Encoding.UTF8.GetBytes(env "JWT_SECRET" "twelve-dollar-challenge")

    let builder = WebApplication.CreateEmptyBuilder(WebApplicationOptions())
    builder.WebHost.UseKestrelCore() |> ignore
    if Uring.supported && env "FS3_TRANSPORT" "uring" = "uring" then
        // Our io_uring transport (Uring.fs) replaces Kestrel's socket transport; it falls back to epoll when the
        // kernel does not allow io_uring.
        builder.Services.AddSingleton<IConnectionListenerFactory, Uring.UringTransportFactory>() |> ignore
    elif Transport.supported then
        // Our epoll transport (Transport.fs) replaces Kestrel's socket transport.
        builder.Services.AddSingleton<IConnectionListenerFactory, Transport.EpollTransportFactory>() |> ignore
    else
        Db.start false
        builder.WebHost.UseSockets(fun o ->
            o.UnsafePreferInlineScheduling <- true // Kestrel's continuations stay on the socket thread
            o.Backlog <- 65535) // the kernel caps it at somaxconn; a ramp of new users should not overflow it
        |> ignore
    builder.WebHost
        .ConfigureKestrel(fun o ->
            o.AddServerHeader <- false
            // Our connection handler goes first and never calls the next one (Kestrel's HTTP layer).
            o.Listen(IPAddress.Parse(env "HOST" "127.0.0.1"), int (env "PORT" "3000"), fun l ->
                l.Use(fun (_: ConnectionDelegate) ->
                    ConnectionDelegate(fun c ->
                        match c with
                        // The epoll loop runs the handler itself and closes the socket.
                        | :? Transport.EConn as e -> if e.Detach() then Task.CompletedTask else e.Closed
                        | _ -> Conn.run c))
                |> ignore))
    |> ignore
    let app = builder.Build()
    let server = app.Services.GetRequiredService<IServer>()
    server.StartAsync(Unused(), CancellationToken.None).GetAwaiter().GetResult()
    Thread.Sleep Timeout.Infinite
    0
