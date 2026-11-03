using System.Net;
using System.Text;
using FeedApi;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.AspNetCore.Server.Kestrel.Transport.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

// Keep every socket completion on the I/O thread that observed it instead of queueing it to
// the thread pool (rule 11: a setting of our own process, set before the first socket exists).
// Together with UnsafePreferInlineScheduling below, a request runs start to finish on one
// thread: no pool hop, no extra scheduler wakeup, no context switch per request.
Environment.SetEnvironmentVariable("DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS", "1");

var sqlitePath = Environment.GetEnvironmentVariable("SQLITE_PATH")
    ?? throw new InvalidOperationException("SQLITE_PATH is not set");
var jwtSecret = Encoding.UTF8.GetBytes(
    Environment.GetEnvironmentVariable("JWT_SECRET") ?? throw new InvalidOperationException("JWT_SECRET is not set"));
var host = Environment.GetEnvironmentVariable("HOST") ?? "127.0.0.1";
if (!int.TryParse(Environment.GetEnvironmentVariable("PORT"), out var port)) port = 3000;

// One core, sub-millisecond handlers: a few pre-started worker threads keep short bursts
// of arrivals from waiting on the pool's injection delay.
ThreadPool.SetMinThreads(4, 4);

var builder = WebApplication.CreateBuilder();
builder.Logging.ClearProviders();
// Kestrel's own continuations run inline on the I/O thread as well. Safe here because
// every step of a request is short (SQLite calls are index lookups in microseconds);
// nothing ever blocks the I/O thread.
builder.Services.Configure<SocketTransportOptions>(
    sockets => sockets.UnsafePreferInlineScheduling = true);
builder.WebHost.ConfigureKestrel(kestrel =>
{
    kestrel.AddServerHeader = false;
    kestrel.Listen(IPAddress.Parse(host), port, listen =>
    {
        listen.Protocols = HttpProtocols.Http1; // the harness speaks HTTP/1.1, skip h2c probing
    });
    kestrel.Limits.MaxRequestBodySize = 1 << 20;
    // KeepAliveTimeout defaults to 130 s, past the 65 s Nginx (or k6) reuses connections for.
});

var app = builder.Build();

using var db = new Db(sqlitePath);
var application = new FeedApp(db, jwtSecret);

// Host Kestrel directly with our IHttpApplication: Kestrel parses HTTP, hands us the raw
// request features, and we write the response bytes. The middleware pipeline, HttpContext
// and routing never run. (This is the hosting IServer.StartAsync was made for.)
var server = app.Services.GetRequiredService<IServer>();
await server.StartAsync(application, CancellationToken.None);
Console.WriteLine($"FeedApi listening on {host}:{port}");

// Everything is committed before it is answered (WAL + synchronous=NORMAL), so there is
// nothing to flush: run until the harness stops us.
await Task.Delay(Timeout.Infinite);
