namespace FeedApi.Middleware;

internal sealed class ApiExceptionMiddleware(RequestDelegate next, ILogger<ApiExceptionMiddleware> logger)
{
    public async Task InvokeAsync(HttpContext context)
    {
        try
        {
            await next(context);
        }
        catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
        {
            context.Abort();
        }
        catch (Exception exception)
        {
            logger.LogError(exception, "Request {Method} {Path} failed", context.Request.Method, context.Request.Path);
            if (context.Response.HasStarted) context.Abort();
            else await ApiResults.Error(500, "internal server error").ExecuteAsync(context);
        }
    }
}
