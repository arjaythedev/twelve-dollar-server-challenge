<?php

return [
    // App\Auth\ChallengeJwtGuard verifies the challenge's HS256 tokens itself: their fixed 23-byte
    // secret is shorter than pype's Jwt accepts (32+ bytes).
    'guards' => [App\Auth\ChallengeJwtGuard::class],

    'jwt' => [
        'secret' => env('JWT_SECRET'),
    ],
];
