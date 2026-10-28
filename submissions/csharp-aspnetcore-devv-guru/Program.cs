using FeedApi;
using FeedApi.Endpoints;
using FeedApi.Middleware;
using Microsoft.AspNetCore.Server.Kestrel.Core;

var options = AppOptions.FromEnvironment();
var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions
{
    Args = [],
    ContentRootPath = AppContext.BaseDirectory,
    EnvironmentName = Environments.Production
});
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole();
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.WebHost.ConfigureKestrel(server =>
{
    server.AddServerHeader = false;
    server.Limits.KeepAliveTimeout = TimeSpan.FromSeconds(130);
    server.Limits.MaxRequestBodySize = 16 * 1024;
    server.Listen(options.Host, options.Port, listen => listen.Protocols = HttpProtocols.Http1);
});
builder.Services.ConfigureHttpJsonOptions(json =>
    json.SerializerOptions.TypeInfoResolverChain.Insert(0, ApiJsonContext.Default));
builder.Services.AddSingleton(options);
builder.Services.AddSingleton<FeedRepository>();
builder.Services.AddSingleton<JwtAuthentication>();

await using var app = builder.Build();
var repository = app.Services.GetRequiredService<FeedRepository>();
_ = app.Services.GetRequiredService<JwtAuthentication>();
var sqliteVersion = repository.Initialize();
app.UseMiddleware<ApiExceptionMiddleware>();
app.MapHealthEndpoints();
app.MapFeedEndpoints();
app.MapPostEndpoints();
// The SDK's route generator loses custom fallback patterns; use the delegate overload.
RequestDelegate notFound = context => ApiResults.Error(404, "not found").ExecuteAsync(context);
app.MapFallback("/{**path}", notFound);
app.Lifetime.ApplicationStarted.Register(() =>
    app.Logger.LogWarning("Feed API listening on {Host}:{Port}; SQLite {Version}; WAL, synchronous=FULL",
        options.Host, options.Port, sqliteVersion));
await app.RunAsync();
