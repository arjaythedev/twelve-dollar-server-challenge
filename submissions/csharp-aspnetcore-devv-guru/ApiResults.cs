namespace FeedApi;

internal static class ApiResults
{
    public static IResult Error(int status, string message) =>
        Results.Json(new ApiError(message), statusCode: status);
}
