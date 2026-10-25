using System.Buffers;
using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

internal sealed record Identity(long UserId, string Username);

internal sealed class TokenAuth(string secret)
{
    private const int MaxTokenLength = 4096;
    private static readonly SearchValues<char> Base64UrlChars =
        SearchValues.Create("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
    private readonly byte[] key = Encoding.UTF8.GetBytes(secret);

    public Identity? Authenticate(string authorization, out string? error)
    {
        error = "missing bearer token";
        if (!authorization.StartsWith("Bearer ", StringComparison.Ordinal)) return null;
        error = "invalid or expired token";
        var token = authorization.AsSpan(7);
        if (token.Length > MaxTokenLength) return null;
        var firstDot = token.IndexOf('.');
        var lastDot = token.LastIndexOf('.');
        if (firstDot < 0 || lastDot == firstDot) return null;
        var header = token[..firstDot];
        var payload = token[(firstDot + 1)..lastDot];
        var signature = token[(lastDot + 1)..];
        // Also rejects a third dot, which would land inside the payload segment.
        if (!IsBase64Url(header) || !IsBase64Url(payload) || !IsBase64Url(signature)) return null;

        Span<byte> buffer = stackalloc byte[MaxTokenLength];
        try
        {
            if (!TryDecode(header, buffer, out var written) || !HasHs256Algorithm(buffer[..written])) return null;

            // No verification cache: recompute the MAC for every authenticated request.
            // The signing input is ASCII (checked above), so each char is one byte.
            var signingInput = token[..lastDot];
            Encoding.ASCII.GetBytes(signingInput, buffer);
            Span<byte> expected = stackalloc byte[HMACSHA256.HashSizeInBytes];
            HMACSHA256.HashData(key, buffer[..signingInput.Length], expected);
            Span<byte> actual = stackalloc byte[Base64Url.GetMaxDecodedLength(signature.Length)];
            if (!TryDecode(signature, actual, out written)
                || !CryptographicOperations.FixedTimeEquals(expected, actual[..written])) return null;

            if (!TryDecode(payload, buffer, out written)) return null;
            return ReadPayload(buffer[..written], ref error);
        }
        catch (JsonException)
        {
            error = "invalid or expired token";
            return null;
        }
    }

    private static bool IsBase64Url(ReadOnlySpan<char> value) =>
        value.Length > 0 && value.Length % 4 != 1 && !value.ContainsAnyExcept(Base64UrlChars);

    private static bool TryDecode(ReadOnlySpan<char> value, Span<byte> destination, out int written) =>
        Base64Url.TryDecodeFromChars(value, destination, out written);

    private static bool HasHs256Algorithm(ReadOnlySpan<byte> json)
    {
        var reader = new Utf8JsonReader(json);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return false;
        var isHs256 = false;
        // Repeated keys: the last one wins, as with JsonElement.TryGetProperty.
        while (reader.Read() && reader.TokenType == JsonTokenType.PropertyName)
        {
            var isAlgorithm = reader.ValueTextEquals("alg"u8);
            reader.Read();
            if (isAlgorithm) isHs256 = reader.TokenType == JsonTokenType.String && reader.ValueTextEquals("HS256"u8);
            reader.Skip();
        }
        // Reading to the end rejects trailing content after the object.
        while (reader.Read()) { }
        return isHs256;
    }

    private static Identity? ReadPayload(ReadOnlySpan<byte> json, ref string? error)
    {
        var reader = new Utf8JsonReader(json);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return null;
        double? expiry = null;
        string? subject = null, username = null;
        while (reader.Read() && reader.TokenType == JsonTokenType.PropertyName)
        {
            if (reader.ValueTextEquals("exp"u8))
            {
                reader.Read();
                expiry = reader.TokenType == JsonTokenType.Number && reader.TryGetDouble(out var seconds) ? seconds : null;
            }
            else if (reader.ValueTextEquals("sub"u8))
            {
                reader.Read();
                subject = reader.TokenType == JsonTokenType.String ? reader.GetString() : null;
            }
            else if (reader.ValueTextEquals("username"u8))
            {
                reader.Read();
                username = reader.TokenType == JsonTokenType.String ? reader.GetString() : null;
            }
            else reader.Read();
            reader.Skip();
        }
        while (reader.Read()) { }

        if (expiry is not { } exp || !double.IsFinite(exp)
            || exp <= DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000d) return null;
        error = "invalid token payload";
        if (!IdParser.TryParse(subject, out var userId) || username is null) return null;
        error = null;
        return new Identity(userId, username);
    }
}
