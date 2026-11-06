<?php

return [
    'default' => 'sqlite',

    'connections' => [
        'sqlite' => [
            'driver' => 'sqlite',
            'database' => env('SQLITE_PATH', 'database/feed.db'),
            // pype's defaults are WAL, synchronous=NORMAL, foreign_keys=ON, busy_timeout=5000,
            // cache_size=-20000 (20 MB), temp_store=MEMORY. The challenge allows mmap too.
            'pragmas' => [
                'mmap_size' => 268435456,   // 256 MB: read pages straight from the OS page cache
            ],
        ],
    ],

    'migrations' => 'database/migrations',
];
