<?php

return [
    'guards' => [App\Auth\ChallengeJwtGuard::class],

    // The challenge's HS256 secret, for App\Auth\ChallengeJwtGuard: a fixed 23 bytes, shorter than
    // pype's Jwt accepts (32+), so it lives under the app's own key, not 'jwt'.
    'challenge' => [
        'secret' => env('JWT_SECRET'),
    ],
];
