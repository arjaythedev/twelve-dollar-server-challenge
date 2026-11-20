<?php

declare(strict_types=1);

namespace App\Auth;

/** The user a valid JWT names. Not looked up: the spec takes `sub` and `username` as given. */
final readonly class TokenUser
{
    public function __construct(public int $id, public string $username)
    {
    }
}
