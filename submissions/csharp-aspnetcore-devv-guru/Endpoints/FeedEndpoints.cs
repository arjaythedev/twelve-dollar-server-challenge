namespace FeedApi.Endpoints;

internal static class FeedEndpoints
{
    public static void MapFeedEndpoints(this IEndpointRouteBuilder endpoints) =>
        endpoints.MapGet("/feed", (FeedRepository repository) => Results.Bytes(repository.GetFeedJson(), "application/json"))
            .WithName("GetFeed").WithTags("Feed");
}
