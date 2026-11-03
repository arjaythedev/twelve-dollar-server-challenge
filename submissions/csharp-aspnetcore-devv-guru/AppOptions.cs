using System.Globalization;
using System.Net;
using System.Text;

namespace FeedApi;

internal sealed record AppOptions(string DatabasePath, string JwtSecret, IPAddress Host, int Port)
{
    public const string ChallengeSecret = "twelve-dollar-challenge";

    public static AppOptions FromEnvironment()
    {
        var path = Environment.GetEnvironmentVariable("SQLITE_PATH");
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            throw new InvalidOperationException("SQLITE_PATH must name an existing database file.");

        var secret = Environment.GetEnvironmentVariable("JWT_SECRET");
        if (string.IsNullOrEmpty(secret))
            throw new InvalidOperationException("JWT_SECRET is required.");
        if (secret != ChallengeSecret && Encoding.UTF8.GetByteCount(secret) < 32)
            throw new InvalidOperationException("JWT_SECRET must contain at least 32 UTF-8 bytes, except for the fixed challenge secret.");

        if (!IPAddress.TryParse(Environment.GetEnvironmentVariable("HOST") ?? "127.0.0.1", out var host))
            throw new InvalidOperationException("HOST must be an IP address.");
        if (!int.TryParse(Environment.GetEnvironmentVariable("PORT") ?? "3000", NumberStyles.None,
                CultureInfo.InvariantCulture, out var port) || port is < 1 or > 65535)
            throw new InvalidOperationException("PORT must be between 1 and 65535.");

        return new AppOptions(Path.GetFullPath(path), secret, host, port);
    }
}
