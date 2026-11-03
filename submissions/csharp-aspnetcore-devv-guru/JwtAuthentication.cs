using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.IdentityModel.JsonWebTokens;
using Microsoft.IdentityModel.Tokens;

namespace FeedApi;

internal sealed record UserIdentity(long Id, string Username);
internal sealed record AuthenticationResult(UserIdentity? Identity, string? Error);

internal sealed class JwtAuthentication
{
    private readonly JsonWebTokenHandler _handler = new() { MaximumTokenSizeInBytes = 16 * 1024 };
    private readonly TokenValidationParameters _parameters;

    public JwtAuthentication(AppOptions options)
    {
        var key = new SymmetricSecurityKey(Encoding.UTF8.GetBytes(options.JwtSecret));
        // The public challenge secret is shorter than IdentityModel's HS256 minimum.
        if (options.JwtSecret == AppOptions.ChallengeSecret)
            key.CryptoProviderFactory = new ChallengeCryptoProviderFactory();

        _parameters = new TokenValidationParameters
        {
            IssuerSigningKey = key,
            ValidAlgorithms = [SecurityAlgorithms.HmacSha256],
            RequireSignedTokens = true,
            ValidateIssuerSigningKey = true,
            ValidateLifetime = true,
            RequireExpirationTime = true,
            ClockSkew = TimeSpan.Zero,
            // Challenge tokens have no issuer or audience.
            ValidateIssuer = false,
            ValidateAudience = false,
            ValidateTokenReplay = false,
            LogValidationExceptions = false,
            IncludeTokenOnFailedValidation = false
        };
    }

    public async Task<AuthenticationResult> AuthenticateAsync(HttpRequest request)
    {
        var authorization = request.Headers.Authorization;
        if (authorization.Count != 1 || !authorization[0]!.StartsWith("Bearer ", StringComparison.Ordinal))
            return new(null, "missing bearer token");

        var encoded = authorization[0]![7..];
        if (string.IsNullOrWhiteSpace(encoded) || encoded.Length > _handler.MaximumTokenSizeInBytes)
            return new(null, "invalid or expired token");
        var validation = await _handler.ValidateTokenAsync(encoded, _parameters);
        if (!validation.IsValid || validation.SecurityToken is not JsonWebToken token)
            return new(null, "invalid or expired token");

        // Claims mapping can coerce numeric values to strings; check the original JSON types.
        using var payload = JsonDocument.Parse(Base64UrlEncoder.DecodeBytes(token.EncodedPayload));
        var root = payload.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("sub", out var sub) || sub.ValueKind != JsonValueKind.String ||
            !long.TryParse(sub.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out var id) || id <= 0 ||
            !root.TryGetProperty("username", out var username) || username.ValueKind != JsonValueKind.String)
            return new(null, "invalid token payload");

        return new(new UserIdentity(id, username.GetString()!), null);
    }

    private sealed class ChallengeCryptoProviderFactory : CryptoProviderFactory
    {
        public override KeyedHashAlgorithm CreateKeyedHashAlgorithm(byte[] keyBytes, string algorithm)
        {
            if (algorithm == SecurityAlgorithms.HmacSha256 &&
                keyBytes.AsSpan().SequenceEqual("twelve-dollar-challenge"u8))
                return new HMACSHA256(keyBytes);
            return base.CreateKeyedHashAlgorithm(keyBytes, algorithm);
        }
    }
}
