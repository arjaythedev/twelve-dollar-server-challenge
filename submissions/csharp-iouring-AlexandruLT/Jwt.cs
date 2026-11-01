using System.Buffers;
using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Feed;
// Verifies every token's HS256 signature, exp and nbf on every request; nothing is cached.
sealed class Jwt(string secret)
{
    readonly byte[] key = Encoding.UTF8.GetBytes(secret);
    public string? VerifyUtf8(ReadOnlySpan<char> authorization, Span<byte> author, bool copyAuthor, out long id, out int authorLength)
    {
        id = 0; authorLength = 0;
        if (!authorization.StartsWith("Bearer ", StringComparison.Ordinal)) return "missing bearer token";
        ReadOnlySpan<char> token = authorization[7..];
        int first = token.IndexOf('.');
        if (first <= 0) return "invalid or expired token";
        int second = token[(first + 1)..].IndexOf('.');
        if (second < 0) return "invalid or expired token";
        second += first + 1;
        ReadOnlySpan<char> h = token[..first], p = token[(first + 1)..second], s = token[(second + 1)..];
        if (!Base64Characters(h) || !Base64Characters(p) || !Base64Characters(s)) return "invalid or expired token";
        byte[]? rented = null;
        Span<byte> work = token.Length <= 1024 ? stackalloc byte[3072] : (rented = ArrayPool<byte>.Shared.Rent(checked(token.Length * 3)));
        try
        {
            Span<byte> header = work[..token.Length], payload = work.Slice(token.Length, token.Length), scratch = work.Slice(token.Length * 2, token.Length);
            int headerLength = Base64Url.DecodeFromChars(h, header);
            if (!Hs256(header[..headerLength])) return "invalid or expired token";
            Span<byte> signature = stackalloc byte[32], expected = stackalloc byte[32];
            if (Base64Url.DecodeFromChars(s, signature) != 32) return "invalid or expired token";
            // Encoded JWT segments are validated ASCII. No concatenated string or byte[].
            for (int n = 0; n < second; n++) scratch[n] = (byte)token[n];
            HMACSHA256.HashData(key, scratch[..second], expected);
            if (!CryptographicOperations.FixedTimeEquals(signature, expected)) return "invalid or expired token";
            int payloadLength = Base64Url.DecodeFromChars(p, payload);
            return ReadPayload(payload[..payloadLength], scratch, author, copyAuthor, out id, out authorLength);
        }
        catch (Exception ex) when (ex is JsonException or FormatException or InvalidOperationException or ArgumentException or DecoderFallbackException or OverflowException)
        { return "invalid or expired token"; }
        finally { if (rented != null) ArrayPool<byte>.Shared.Return(rented, clearArray: true); }
    }
    static bool Base64Characters(ReadOnlySpan<char> value)
    {
        if (value.IsEmpty) return false;
        foreach (char c in value) if (!(c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_')) return false;
        return true;
    }
    static bool Hs256(ReadOnlySpan<byte> bytes)
    {
        JsonInput.Validate(bytes);
        var reader = new Utf8JsonReader(bytes);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return false;
        bool algorithm = false;
        while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
        {
            if (reader.TokenType != JsonTokenType.PropertyName) throw new JsonException();
            bool match = reader.ValueTextEquals("alg"u8);
            if (!reader.Read()) throw new JsonException();
            if (match) algorithm = reader.TokenType == JsonTokenType.String && reader.ValueTextEquals("HS256"u8);
            reader.Skip();
        }
        return reader.TokenType == JsonTokenType.EndObject && !reader.Read() && algorithm;
    }
    static string? ReadPayload(ReadOnlySpan<byte> bytes, Span<byte> scratch, Span<byte> author, bool copyAuthor, out long id, out int authorLength)
    {
        id = 0; authorLength = 0;
        JsonInput.Validate(bytes);
        var reader = new Utf8JsonReader(bytes);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) throw new JsonException();
        bool expirationValid = false, beforeValid = true, subjectValid = false, nameValid = false;
        long expiration = 0, before = 0;
        while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
        {
            if (reader.TokenType != JsonTokenType.PropertyName) throw new JsonException();
            int field = reader.ValueTextEquals("exp"u8) ? 1 : reader.ValueTextEquals("nbf"u8) ? 2
                : reader.ValueTextEquals("sub"u8) ? 3 : reader.ValueTextEquals("username"u8) ? 4 : 0;
            if (!reader.Read()) throw new JsonException();
            if (field == 1) expirationValid = reader.TokenType == JsonTokenType.Number && reader.TryGetInt64(out expiration);
            else if (field == 2) beforeValid = reader.TokenType == JsonTokenType.Number && reader.TryGetInt64(out before);
            else if (field == 3)
            {
                subjectValid = false;
                if (reader.TokenType == JsonTokenType.String)
                {
                    int length = reader.CopyString(scratch);
                    subjectValid = PositiveUtf8(scratch[..length], out id);
                }
            }
            else if (field == 4)
            {
                nameValid = reader.TokenType == JsonTokenType.String; authorLength = 0;
                if (nameValid)
                {
                    int length = reader.CopyString(scratch);
                    if (copyAuthor) { scratch[..length].CopyTo(author); authorLength = length; }
                }
            }
            reader.Skip();
        }
        if (reader.TokenType != JsonTokenType.EndObject || reader.Read()) throw new JsonException();
        long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        if (!expirationValid || expiration <= now || !beforeValid || before > now) return "invalid or expired token";
        return subjectValid && nameValid ? null : "invalid token payload";
    }
    public static bool PositiveUtf8(ReadOnlySpan<byte> value, out long id)
    {
        id = 0;
        if (value.IsEmpty) return false;
        foreach (byte b in value) if (b is < (byte)'0' or > (byte)'9') return false;
        return Utf8Parser.TryParse(value, out id, out int consumed) && consumed == value.Length && id > 0;
    }
}
