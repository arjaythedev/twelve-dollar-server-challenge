<?php

declare(strict_types=1);

namespace App\Security;

use Symfony\Component\Security\Core\Exception\AuthenticationException;

/** An authentication failure whose message is exactly what the API must answer with. */
final class AuthError extends AuthenticationException
{
    public function getMessageKey(): string
    {
        return $this->getMessage();
    }
}
