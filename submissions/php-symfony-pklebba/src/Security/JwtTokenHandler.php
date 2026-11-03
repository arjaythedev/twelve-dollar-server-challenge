<?php

declare(strict_types=1);

namespace App\Security;

use Symfony\Component\DependencyInjection\Attribute\Autowire;
use Symfony\Component\Security\Http\AccessToken\AccessTokenHandlerInterface;
use Symfony\Component\Security\Http\Authenticator\Passport\Badge\UserBadge;

/**
 * Verifies the bearer token on every request (rule 5: nothing is remembered between requests).
 *
 * HS256 only: the signature is recomputed with hash_hmac and compared in constant time, then "exp"
 * (and "nbf", if present) is checked. This is done by hand rather than with firebase/php-jwt or
 * lcobucci/jwt: both refuse HMAC keys shorter than 256 bits, and the challenge secret is 23 bytes.
 */
final class JwtTokenHandler implements AccessTokenHandlerInterface
{
    public function __construct(
        #[Autowire(env: 'JWT_SECRET')]
        private readonly string $secret,
    ) {
    }

    public function getUserBadgeFrom(#[\SensitiveParameter] string $accessToken): UserBadge
    {
        $parts = explode('.', $accessToken);
        if (3 !== \count($parts)) {
            throw new AuthError('invalid or expired token');
        }
        [$header64, $payload64, $signature64] = $parts;

        $header = json_decode(self::base64UrlDecode($header64), true);
        if (!\is_array($header) || 'HS256' !== ($header['alg'] ?? null)) {
            throw new AuthError('invalid or expired token');
        }

        $signature = self::base64UrlDecode($signature64);
        if ('' === $signature || !hash_equals(hash_hmac('sha256', $header64.'.'.$payload64, $this->secret, true), $signature)) {
            throw new AuthError('invalid or expired token');
        }

        $claims = json_decode(self::base64UrlDecode($payload64), true);
        if (!\is_array($claims)) {
            throw new AuthError('invalid or expired token');
        }
        $now = time();
        $exp = $claims['exp'] ?? null;
        if (!\is_int($exp) && !\is_float($exp) || $exp <= $now) {
            throw new AuthError('invalid or expired token');
        }
        $nbf = $claims['nbf'] ?? null;
        if (null !== $nbf && (!\is_int($nbf) && !\is_float($nbf) || $nbf > $now)) {
            throw new AuthError('invalid or expired token');
        }

        $sub = $claims['sub'] ?? null;
        $username = $claims['username'] ?? null;
        if (!\is_string($sub) || !preg_match('/^[1-9][0-9]{0,17}$/D', $sub) || !\is_string($username)) {
            throw new AuthError('invalid token payload');
        }

        return new UserBadge($sub, static fn (): JwtUser => new JwtUser((int) $sub, $username));
    }

    private static function base64UrlDecode(string $input): string
    {
        return (string) base64_decode(strtr($input, '-_', '+/'), true);
    }
}
