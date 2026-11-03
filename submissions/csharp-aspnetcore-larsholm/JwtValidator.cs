using System.Buffers.Text;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Feed;

public sealed record AuthUser(long Id, string Username);

public sealed record AuthResult(AuthUser? User, string? Error);

/// <summary>
/// Verifies HS256 bearer tokens. Microsoft.IdentityModel refuses HMAC keys under 256 bits and the
/// challenge secret is 184 bits, so the signature is checked here directly with HMACSHA256.
/// </summary>
public sealed class JwtValidator(string secret)
{
    private readonly byte[] _key = Encoding.UTF8.GetBytes(secret);

    private static readonly AuthResult Missing = new(null, "missing bearer token");
    private static readonly AuthResult Invalid = new(null, "invalid or expired token");
    private static readonly AuthResult BadPayload = new(null, "invalid token payload");

    public AuthResult Validate(string? authorization)
    {
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal))
            return Missing;

        var parts = authorization["Bearer ".Length..].Split('.');
        if (parts.Length != 3)
            return Invalid;

        try
        {
            using var header = JsonDocument.Parse(Base64Url.DecodeFromChars(parts[0]));
            if (header.RootElement.GetProperty("alg").GetString() != "HS256")
                return Invalid;

            var expected = HMACSHA256.HashData(_key, Encoding.ASCII.GetBytes($"{parts[0]}.{parts[1]}"));
            if (!CryptographicOperations.FixedTimeEquals(expected, Base64Url.DecodeFromChars(parts[2])))
                return Invalid;

            using var payload = JsonDocument.Parse(Base64Url.DecodeFromChars(parts[1]));
            var claims = payload.RootElement;
            if (!claims.TryGetProperty("exp", out var exp) || exp.GetInt64() <= DateTimeOffset.UtcNow.ToUnixTimeSeconds())
                return Invalid;

            if (claims.TryGetProperty("sub", out var sub) && sub.ValueKind == JsonValueKind.String
                && long.TryParse(sub.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out var userId) && userId > 0
                && claims.TryGetProperty("username", out var username) && username.ValueKind == JsonValueKind.String)
                return new AuthResult(new AuthUser(userId, username.GetString()!), null);

            return BadPayload;
        }
        catch (Exception ex) when (ex is FormatException or JsonException or InvalidOperationException or KeyNotFoundException)
        {
            return Invalid;
        }
    }
}
