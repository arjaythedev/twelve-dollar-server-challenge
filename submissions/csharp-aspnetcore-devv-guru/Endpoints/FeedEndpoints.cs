namespace FeedApi.Endpoints;

internal static class FeedEndpoints
{
    public static void MapFeedEndpoints(this IEndpointRouteBuilder endpoints) =>
        endpoints.MapGet("/feed", GetFeedAsync)
            .WithName("GetFeed").WithTags("Feed");

    private static async Task GetFeedAsync(HttpContext context, FeedRepository repository)
    {
        var length = repository.CopyFeedJson(context.Response.BodyWriter);
        context.Response.ContentType = "application/json";
        context.Response.ContentLength = length;
        await context.Response.BodyWriter.FlushAsync(context.RequestAborted);
    }
}
