<?php

declare(strict_types=1);

namespace App\Security;

use Symfony\Component\Security\Core\User\UserInterface;

/** The user as described by the JWT: the id from "sub" and "username" as-is (never looked up, per the spec). */
final class JwtUser implements UserInterface
{
    public function __construct(
        public readonly int $id,
        public readonly string $username,
    ) {
    }

    public function getRoles(): array
    {
        return ['ROLE_USER'];
    }

    public function getUserIdentifier(): string
    {
        return (string) $this->id;
    }
}
