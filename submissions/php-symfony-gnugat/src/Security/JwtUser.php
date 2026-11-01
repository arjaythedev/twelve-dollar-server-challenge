<?php

declare(strict_types=1);

namespace App\Security;

use Symfony\Component\Security\Core\User\UserInterface;

/**
 * The user as described by a verified token: the username is used as-is, never looked up.
 */
final readonly class JwtUser implements UserInterface
{
    public function __construct(
        public int $id,
        public string $username,
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
