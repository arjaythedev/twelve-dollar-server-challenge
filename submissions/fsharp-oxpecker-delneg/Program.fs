module Program

open System
open System.Net
open System.Text
open Microsoft.AspNetCore.Builder
open Microsoft.AspNetCore.Hosting
open Microsoft.AspNetCore.Server.Kestrel.Core
open Microsoft.Extensions.DependencyInjection
open Oxpecker

let endpoints = [
    GET [
        route "/health" (Api.guard Api.health)
        route "/feed" (Api.guard Api.feed)
        route "/posts/{id}" (Api.guard Api.getPost)
    ]
    POST [
        route "/posts" (Api.guard Api.createPost)
        route "/posts/{id}/like" (Api.guard Api.like)
    ]
]

let env name fallback =
    match Environment.GetEnvironmentVariable name with
    | null
    | "" -> fallback
    | v -> v

[<EntryPoint>]
let main args =
    // Socket completions run on .NET's socket thread, not a thread-pool worker: on one core that hand-off
    // is a context switch per request. Read when the first socket is created, so set it first.
    Environment.SetEnvironmentVariable("DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS", "1")
    Db.openDb (env "SQLITE_PATH" "feed.db")
    Auth.key <- Encoding.UTF8.GetBytes(env "JWT_SECRET" "twelve-dollar-challenge")

    let builder = WebApplication.CreateEmptyBuilder(WebApplicationOptions(Args = args))
    builder.WebHost
        .UseKestrelCore()
        .UseSockets(fun o ->
            o.UnsafePreferInlineScheduling <- true // Kestrel's continuations stay on the socket thread
            o.Backlog <- 65535) // the kernel caps it at somaxconn; a ramp of new users should not overflow it
        .ConfigureKestrel(fun o ->
            o.AddServerHeader <- false
            o.Limits.KeepAliveTimeout <- TimeSpan.FromSeconds 120.0 // SPEC: keep idle connections at least 65 s
            o.Limits.MinRequestBodyDataRate <- null
            o.Listen(IPAddress.Parse(env "HOST" "127.0.0.1"), int (env "PORT" "3000"), fun l -> l.Protocols <- HttpProtocols.Http1))
    |> ignore
    builder.Services.AddRoutingCore() |> ignore
    let app = builder.Build()
    app.UseRouting().UseOxpecker(endpoints).Run(Api.notFound)
    app.Run()
    0
