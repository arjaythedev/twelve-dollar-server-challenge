<?php

declare(strict_types=1);

namespace App\Auth;

use Pype\Application;
use Pype\Auth\Guard;
use Pype\Exceptions\HttpException;
use Pype\Http\Request;

/**
 * The challenge's tokens: HS256 only, signed with a fixed 23-byte JWT_SECRET. pype's Jwt (lcobucci)
 * refuses HS256 keys under 32 bytes (RFC 7518), so this guard verifies them natively: the header
 * must say HS256, the signature must match (hash_equals), `exp` must be in the future. The user
 * is the token's `sub` (a positive integer string) and `username` (a string), taken as given.
 * The error keys are the framework's, worded in config/app.php 'errors'.
 */
final class ChallengeJwtGuard implements Guard
{
    private readonly string $secret;

    public function __construct(Application $app)
    {
        $this->secret = (string) $app->config('auth.jwt.secret', '');
    }

    public function authenticate(Request $request): ?object
    {
        $header = (string) $request->header('authorization');

        if (strncmp($header, 'Bearer ', 7) !== 0) {
            return null;   // no guard accepts it: 401 'unauthenticated'
        }

        $claims = $this->verify(substr($header, 7)) ?? throw HttpException::unauthorized('Invalid or expired token')->withKey('invalid_token');
        $sub = $claims['sub'] ?? null;
        $username = $claims['username'] ?? null;

        return is_string($sub) && preg_match('/^[1-9]\d*$/', $sub) && is_string($username)
            ? new TokenUser((int) $sub, $username)
            : throw HttpException::unauthorized('Invalid token payload')->withKey('invalid_token_payload');
    }

    /** @return array<string, mixed>|null  the claims of a valid, unexpired HS256 token */
    private function verify(string $token): ?array
    {
        $parts = explode('.', $token);

        if (count($parts) !== 3 || $this->secret === '') {
            return null;
        }

        $head = self::json($parts[0]);
        $claims = self::json($parts[1]);
        $signature = self::decode($parts[2]);

        if (($head['alg'] ?? null) !== 'HS256' || $claims === null || $signature === null
            || !hash_equals(hash_hmac('sha256', $parts[0] . '.' . $parts[1], $this->secret, true), $signature)) {
            return null;
        }

        $exp = $claims['exp'] ?? null;

        return (is_int($exp) || is_float($exp)) && $exp > time() ? $claims : null;
    }

    private static function json(string $part): ?array
    {
        $json = self::decode($part);
        $data = $json === null ? null : json_decode($json, true);

        return is_array($data) ? $data : null;
    }

    private static function decode(string $part): ?string
    {
        $raw = base64_decode(strtr($part, '-_', '+/'), true);

        return $raw === false ? null : $raw;
    }
}
