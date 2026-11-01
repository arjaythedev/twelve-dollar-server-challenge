<?php

declare(strict_types=1);

namespace App\Security;

use Symfony\Component\DependencyInjection\Attribute\Autowire;
use Symfony\Component\Security\Core\Exception\CustomUserMessageAuthenticationException;
use Symfony\Component\Security\Http\AccessToken\AccessTokenHandlerInterface;
use Symfony\Component\Security\Http\Authenticator\Passport\Badge\UserBadge;

/**
 * Verifies HS256 JWTs (signature, expiry and not-before) on every request.
 *
 * Verified by hand: lcobucci/jwt and firebase/php-jwt 7 both refuse HMAC keys shorter than 256 bits
 * (RFC 7518), and the challenge's JWT_SECRET is 23 bytes.
 */
final readonly class JwtAccessTokenHandler implements AccessTokenHandlerInterface
{
    public function __construct(
        #[Autowire('%env(JWT_SECRET)%')]
        #[\SensitiveParameter]
        private string $secret,
    ) {
    }

    public function getUserBadgeFrom(#[\SensitiveParameter] string $accessToken): UserBadge
    {
        $claims = $this->verify($accessToken)
            ?? throw new CustomUserMessageAuthenticationException('invalid or expired token');

        $sub = $claims['sub'] ?? null;
        $username = $claims['username'] ?? null;
        if (!\is_string($sub) || !preg_match('/^[1-9][0-9]*$/D', $sub) || !\is_string($username)) {
            throw new CustomUserMessageAuthenticationException('invalid token payload');
        }

        $jwtUser = new JwtUser((int) $sub, $username);

        return new UserBadge($jwtUser->getUserIdentifier(), static fn (): JwtUser => $jwtUser);
    }

    /**
     * @return array<mixed>|null the claims, or null if the token is malformed, not HS256, forged, expired or not yet valid
     */
    private function verify(string $jwt): ?array
    {
        $segments = explode('.', $jwt);
        if (3 !== \count($segments)) {
            return null;
        }

        [$header, $payload, $signature] = $segments;

        if ('HS256' !== ($this->decodeJson($header)['alg'] ?? null)) {
            return null;
        }

        $expected = hash_hmac('sha256', $header.'.'.$payload, $this->secret, true);
        if (!hash_equals($expected, $this->decodeBase64Url($signature) ?? '')) {
            return null;
        }

        $claims = $this->decodeJson($payload);
        $now = time();
        if (null === $claims || !\is_int($claims['exp'] ?? null) || $claims['exp'] <= $now) {
            return null;
        }

        // "nbf" (not before, optional): the token must be rejected until that Unix time
        if (\array_key_exists('nbf', $claims) && (!\is_int($claims['nbf']) || $claims['nbf'] > $now)) {
            return null;
        }

        return $claims;
    }

    /**
     * @return array<mixed>|null
     */
    private function decodeJson(string $segment): ?array
    {
        $json = $this->decodeBase64Url($segment);
        $data = null === $json ? null : json_decode($json, true);

        return \is_array($data) ? $data : null;
    }

    private function decodeBase64Url(string $segment): ?string
    {
        $decoded = base64_decode(strtr($segment, '-_', '+/'), true);

        return false === $decoded ? null : $decoded;
    }
}
