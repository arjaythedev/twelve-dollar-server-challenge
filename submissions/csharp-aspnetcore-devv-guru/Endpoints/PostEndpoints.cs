using System.Globalization;
using System.Text.Json;
using static FeedApi.ApiResults;

namespace FeedApi.Endpoints;

internal static class PostEndpoints
{
    public static void MapPostEndpoints(this IEndpointRouteBuilder endpoints)
    {
        var posts = endpoints.MapGroup("/posts").WithTags("Posts");
        posts.MapGet("/{id}", GetPost).WithName("GetPost");
        posts.MapPost("", CreatePostAsync).WithName("CreatePost");
        posts.MapPost("/{id}/like", LikePostAsync).WithName("LikePost");
    }

    private static IResult GetPost(string id, FeedRepository repository)
    {
        if (!TryPostId(id, out var postId)) return Error(400, "invalid post id");
        var post = repository.GetPost(postId);
        return post is null ? Error(404, "post not found") : Results.Json(new PostResponse(post));
    }

    private static async Task<IResult> CreatePostAsync(
        HttpContext context, FeedRepository repository, JwtAuthentication authentication)
    {
        var auth = await authentication.AuthenticateAsync(context.Request);
        if (auth.Error is not null) return Error(401, auth.Error);

        string? body;
        try
        {
            using var document = await JsonDocument.ParseAsync(context.Request.Body, cancellationToken: context.RequestAborted);
            var root = document.RootElement;
            body = root.ValueKind == JsonValueKind.Object
                && root.TryGetProperty("body", out var value)
                && value.ValueKind == JsonValueKind.String
                    ? value.GetString()?.Trim()
                    : null;
        }
        catch (JsonException)
        {
            return Error(400, "malformed JSON body");
        }
        if (string.IsNullOrEmpty(body)) return Error(400, "body is required");
        // SQLite length() counts Unicode code points, rather than UTF-16 code units.
        if (body.EnumerateRunes().Count() > 500) return Error(400, "body must be at most 500 characters");

        var post = await repository.CreatePostAsync(auth.Identity!, body, context.RequestAborted);
        return Results.Json(new PostResponse(post), statusCode: 201);
    }

    private static async Task<IResult> LikePostAsync(
        string id, HttpContext context, FeedRepository repository, JwtAuthentication authentication)
    {
        var auth = await authentication.AuthenticateAsync(context.Request);
        if (auth.Error is not null) return Error(401, auth.Error);
        if (!TryPostId(id, out var postId)) return Error(400, "invalid post id");
        var outcome = await repository.LikeAsync(auth.Identity!.Id, postId, context.RequestAborted);
        return outcome == LikeOutcome.MissingPost
            ? Error(404, "post not found")
            : Results.Json(new LikeResponse(true, outcome == LikeOutcome.AlreadyLiked, postId),
                statusCode: outcome == LikeOutcome.Created ? 201 : 200);
    }

    private static bool TryPostId(string text, out long id) =>
        long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;
}
