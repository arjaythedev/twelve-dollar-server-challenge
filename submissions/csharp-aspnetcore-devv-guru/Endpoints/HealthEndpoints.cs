using System.Diagnostics;

namespace FeedApi.Endpoints;

internal static class HealthEndpoints
{
    public static void MapHealthEndpoints(this IEndpointRouteBuilder endpoints)
    {
        var started = Stopwatch.GetTimestamp();
        var logger = endpoints.ServiceProvider.GetRequiredService<ILoggerFactory>().CreateLogger("FeedApi.Health");
        endpoints.MapGet("/health", (FeedRepository repository) =>
        {
            try
            {
                repository.CheckHealth();
                return Results.Json(new HealthResponse("ok", "ok", (long)Stopwatch.GetElapsedTime(started).TotalSeconds),
                    ApiJsonContext.Default.HealthResponse);
            }
            catch (Exception exception)
            {
                logger.LogError(exception, "Database health check failed");
                return Results.Json(new DegradedHealthResponse("degraded", "unreachable", "database check failed"),
                    ApiJsonContext.Default.DegradedHealthResponse, statusCode: 503);
            }
        }).WithName("GetHealth").WithTags("Health");
    }
}
