namespace FeedApi.Endpoints;

internal static class FeedEndpoints
{
    public static void MapFeedEndpoints(this IEndpointRouteBuilder endpoints) =>
        endpoints.MapGet("/feed", (FeedRepository repository) => Results.Json(new FeedResponse(repository.GetFeed())))
            .WithName("GetFeed").WithTags("Feed");
}
