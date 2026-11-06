<?php

return [
    // Lowest level written: debug, info, notice, warning, error, critical, alert, emergency.
    'level' => env('LOG_LEVEL', 'debug'),

    // One JSON line per entry, a file per day: storage/logs/pype-YYYY-MM-DD.log
    'path' => env('LOG_PATH', 'storage/logs'),
    'days' => 14,   // older files are deleted

    // Also send entries elsewhere: 'stderr' / 'stdout' (JSON lines for Docker, Kubernetes and
    // platform log collectors), 'syslog', or the class name of any PSR-3 logger (e.g. a Monolog
    // logger with a Sentry handler). The file above is always written too, for the log viewer.
    // Prefer stderr/syslog + a log agent over calling a web service on every log line.
    'forward' => array_values(array_filter(explode(',', (string) env('LOG_FORWARD', '')))),
    'forward_level' => env('LOG_FORWARD_LEVEL', 'debug'),   // e.g. 'warning' to keep Sentry quiet

    // A line per request ("GET /expenses 200 3.1 ms"). Handy in development; a file append per request.
    'requests' => (bool) env('LOG_REQUESTS', env('APP_DEBUG', false)),

    // The log viewer: live entries with filters at /_pype/logs.
    // On by default only when APP_DEBUG=true. In production it is only registered if enabled AND
    // given stages that guard it, e.g. ['web', 'auth', App\Stages\EnsureAdmin::class]: logs can
    // contain personal data and errors.
    'viewer' => [
        'enabled' => (bool) env('LOG_VIEWER', env('APP_DEBUG', false)),
        'path' => '/_pype/logs',
        'stages' => [],
    ],
];
