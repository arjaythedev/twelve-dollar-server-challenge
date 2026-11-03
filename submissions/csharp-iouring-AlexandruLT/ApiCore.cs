using System.Diagnostics;

namespace Feed;
enum ApiRoute { Missing, Health, Feed, Post, Create, Like }

// Runs one request against SQLite and writes its JSON body into output.
static class ApiCore
{
    static readonly Dictionary<string, byte[]> Errors = new(StringComparer.Ordinal)
    {
        ["not found"] = "{\"error\":\"not found\"}"u8.ToArray(),
        ["post not found"] = "{\"error\":\"post not found\"}"u8.ToArray(),
        ["invalid post id"] = "{\"error\":\"invalid post id\"}"u8.ToArray(),
        ["missing bearer token"] = "{\"error\":\"missing bearer token\"}"u8.ToArray(),
        ["invalid or expired token"] = "{\"error\":\"invalid or expired token\"}"u8.ToArray(),
        ["invalid token payload"] = "{\"error\":\"invalid token payload\"}"u8.ToArray(),
        ["malformed JSON body"] = "{\"error\":\"malformed JSON body\"}"u8.ToArray(),
        ["body is required"] = "{\"error\":\"body is required\"}"u8.ToArray(),
        ["body must be at most 500 characters"] = "{\"error\":\"body must be at most 500 characters\"}"u8.ToArray(),
        ["body too large"] = "{\"error\":\"body too large\"}"u8.ToArray(),
        ["internal server error"] = "{\"error\":\"internal server error\"}"u8.ToArray(),
        ["bad request"] = "{\"error\":\"bad request\"}"u8.ToArray(),
        ["headers too large"] = "{\"error\":\"headers too large\"}"u8.ToArray(),
    };
    public static ReadOnlySpan<byte> ErrorBytes(string message) => Errors[message];
    static int Error(Output output, int status, string message)
    { output.Reset(); output.Raw(ErrorBytes(message)); return status; }

    public static int Execute(Db db, Output output, ApiRoute route, long id, long user,
        ReadOnlySpan<byte> body, ReadOnlySpan<byte> author, long started)
    {
        output.Reset();
        switch (route)
        {
            case ApiRoute.Health:
                try
                {
                    db.Health();
                    output.Raw("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"u8);
                    output.Number((long)Stopwatch.GetElapsedTime(started).TotalSeconds); output.Raw("}"u8);
                    return 200;
                }
                catch
                {
                    output.Reset(); output.Raw("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"SELECT 1 failed\"}"u8); return 503;
                }
            case ApiRoute.Feed: db.Read(output, null); return 200;
            case ApiRoute.Post: return db.Read(output, id) ? 200 : Error(output, 404, "post not found");
            case ApiRoute.Create: db.Create(output, user, body, author); return 201;
            case ApiRoute.Like:
                int status = db.Like(user, id);
                if (status == 404) return Error(output, 404, "post not found");
                output.Raw(status == 201 ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"u8 : "{\"liked\":true,\"already_liked\":true,\"post_id\":"u8);
                output.Number(id); output.Raw("}"u8);
                return status;
            default: return Error(output, 404, "not found");
        }
    }
}
