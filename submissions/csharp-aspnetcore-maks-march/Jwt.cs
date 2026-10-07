using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.AspNetCore.Http;

namespace FeedApi;

internal enum AuthStatus
{
    Ok,
    MissingToken,   // no Authorization header, or it doesn't start with "Bearer "
    InvalidToken,   // bad signature, expired, not HS256, or malformed JWT
    BadPayload,     // valid JWT, but sub isn't a positive integer string or username isn't a string
}

internal readonly struct AuthResult(AuthStatus status, long userId, string username)
{
    public AuthStatus Status { get; } = status;
    public long UserId { get; } = userId;
    public string Username { get; } = username;
}

/// <summary>
/// Hand-rolled HS256 verification, run on every authenticated request (rule 5: no remembered
/// verifications). Stackalloc buffers, a thread-static HMAC, a hand-rolled base64url decoder
/// and a Utf8JsonReader scan of the payload: no per-request allocations except the username.
/// </summary>
internal static class Jwt
{
    private const int MaxSegmentChars = 1024;

    [ThreadStatic] private static HMACSHA256? _hmac;

    public static AuthResult Verify(IHeaderDictionary headers, byte[] secret)
    {
        var auth = headers.Authorization;
        if (auth.Count == 0) return new AuthResult(AuthStatus.MissingToken, 0, "");
        var header = auth.ToString();
        if (!header.StartsWith("Bearer ", StringComparison.Ordinal))
            return new AuthResult(AuthStatus.MissingToken, 0, "");

        var token = header.Substring(7);
        int dot1 = token.IndexOf('.');
        if (dot1 < 0) return Invalid;
        int dot2 = token.IndexOf('.', dot1 + 1);
        if (dot2 < 0 || token.IndexOf('.', dot2 + 1) >= 0) return Invalid;

        // Header: must be a JSON object with alg == HS256 (checked before the signature,
        // exactly like the reference implementation: alg=none is "invalid", never verified).
        Span<byte> head = stackalloc byte[512];
        if (!TryB64UrlDecode(token.AsSpan(0, dot1), head, out int headLen)) return Invalid;
        if (!IsHs256Header(head[..headLen])) return Invalid;

        // Signature over the ASCII bytes "header.payload" exactly as they appear in the token.
        if (dot2 > MaxSegmentChars) return Invalid;
        Span<byte> signed = stackalloc byte[dot2];
        for (int i = 0; i < dot2; i++)
        {
            char c = token[i];
            bool ok = c == '.' || c == '-' || c == '_'
                || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!ok) return Invalid;
            signed[i] = (byte)c;
        }
        Span<byte> sig = stackalloc byte[48];
        if (!TryB64UrlDecode(token.AsSpan(dot2 + 1), sig, out int sigLen) || sigLen != 32) return Invalid;

        var hmac = _hmac ??= new HMACSHA256(secret);
        hmac.Initialize();
        Span<byte> digest = stackalloc byte[32];
        hmac.TryComputeHash(signed, digest, out _);
        if (!CryptographicOperations.FixedTimeEquals(sig[..32], digest)) return Invalid;

        Span<byte> payload = stackalloc byte[768];
        if (!TryB64UrlDecode(token.AsSpan(dot1 + 1, dot2 - dot1 - 1), payload, out int payloadLen))
            return Invalid;
        return ParsePayload(payload[..payloadLen]);
    }

    private static AuthResult Invalid => new(AuthStatus.InvalidToken, 0, "");
    private static AuthResult BadPayload => new(AuthStatus.BadPayload, 0, "");

    private static bool IsHs256Header(ReadOnlySpan<byte> json)
    {
        try
        {
            var reader = new Utf8JsonReader(json);
            if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return false;
            while (reader.Read())
            {
                if (reader.TokenType == JsonTokenType.EndObject) return false;
                if (reader.TokenType != JsonTokenType.PropertyName) return false;
                if (reader.ValueTextEquals("alg"u8))
                {
                    return reader.Read()
                        && reader.TokenType == JsonTokenType.String
                        && reader.ValueTextEquals("HS256"u8);
                }
                if (!reader.Read()) return false;
                reader.Skip();
            }
            return false;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static AuthResult ParsePayload(ReadOnlySpan<byte> json)
    {
        long userId = 0;
        string username = "";
        bool sawSub = false, sawUsername = false;
        long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

        try
        {
            var reader = new Utf8JsonReader(json);
            if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return Invalid;

            while (reader.Read())
            {
                if (reader.TokenType == JsonTokenType.EndObject) break;
                if (reader.TokenType != JsonTokenType.PropertyName) return Invalid;

                bool isExp = reader.ValueTextEquals("exp"u8);
                bool isNbf = !isExp && reader.ValueTextEquals("nbf"u8);
                bool isSub = !isExp && !isNbf && reader.ValueTextEquals("sub"u8);
                bool isUsername = !isExp && !isNbf && !isSub && reader.ValueTextEquals("username"u8);

                if (!reader.Read()) return Invalid;
                var kind = reader.TokenType;

                if (isExp || isNbf)
                {
                    if (kind == JsonTokenType.Null) continue; // treated as absent
                    if (kind != JsonTokenType.Number) return Invalid;
                    double moment = reader.GetDouble();
                    bool outOfWindow = isExp ? now >= moment : now < moment;
                    if (outOfWindow) return Invalid;
                }
                else if (isSub)
                {
                    if (kind != JsonTokenType.String) return BadPayload;
                    if (!TryParseSub(reader.GetString(), out userId)) return BadPayload;
                    sawSub = true;
                }
                else if (isUsername)
                {
                    if (kind != JsonTokenType.String) return BadPayload;
                    username = reader.GetString()!;
                    sawUsername = true;
                }
                else
                {
                    reader.Skip();
                }
            }
        }
        catch (JsonException)
        {
            return Invalid;
        }

        if (!sawSub || !sawUsername) return BadPayload;
        return new AuthResult(AuthStatus.Ok, userId, username);
    }

    /// A positive integer in plain decimal digits that fits SQLite's INTEGER range.
    private static bool TryParseSub(string? sub, out long userId)
    {
        userId = 0;
        if (string.IsNullOrEmpty(sub)) return false;
        foreach (var c in sub)
        {
            if (c < '0' || c > '9') return false;
        }
        if (!long.TryParse(sub, NumberStyles.None, CultureInfo.InvariantCulture, out userId)) return false;
        return userId > 0;
    }

    /// Base64url without padding; rejects any other character (like the reference's regex).
    private static bool TryB64UrlDecode(ReadOnlySpan<char> src, Span<byte> dst, out int written)
    {
        written = 0;
        int len = src.Length;
        if (len == 0 || (len & 3) == 1) return false;
        int expected = (len / 4) * 3 + (len % 4 == 2 ? 1 : len % 4 == 3 ? 2 : 0);
        if (expected > dst.Length) return false;

        int acc = 0, bits = 0, o = 0;
        for (int i = 0; i < len; i++)
        {
            int v = src[i] switch
            {
                >= 'A' and <= 'Z' => src[i] - 'A',
                >= 'a' and <= 'z' => src[i] - 'a' + 26,
                >= '0' and <= '9' => src[i] - '0' + 52,
                '-' => 62,
                '_' => 63,
                _ => -1,
            };
            if (v < 0) return false;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                dst[o++] = (byte)(acc >> bits);
            }
        }
        written = o;
        return true;
    }
}
