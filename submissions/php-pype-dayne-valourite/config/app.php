<?php

return [
    'name' => 'twelve-pype',
    'debug' => (bool) env('APP_DEBUG', false),

    // 'fpm' or 'swoole' (`php bin/pype serve`): the benchmark runs OpenSwoole.
    'server' => env('APP_SERVER', 'swoole'),
    // Any SWOOLE_* variable sets an OpenSwoole setting (SWOOLE_MODE=base, SWOOLE_BACKLOG=8192, ...).
    'swoole' => [
        'workers' => env('SWOOLE_WORKERS'),   // default: 1 per CPU core
        'max_request' => 0,                   // never recycle workers: the app holds no per-request state
        ...env_prefixed('SWOOLE_'),
    ],

    // The spec's wording for pype's own errors.
    'errors' => [
        'no_route' => 'not found',
        'bad_json' => 'malformed JSON body',
        'unauthenticated' => 'missing bearer token',
        'invalid_token' => 'invalid or expired token',
        'invalid_token_payload' => 'invalid token payload',
        'server_error' => 'internal server error',
    ],

    // Only what this API uses: no sessions, views, mail, cache or queue.
    'providers' => [
        Pype\Log\LogProvider::class,
        Pype\Database\DatabaseProvider::class,
        Pype\Auth\AuthProvider::class,
        App\Providers\AppProvider::class,
    ],
];
