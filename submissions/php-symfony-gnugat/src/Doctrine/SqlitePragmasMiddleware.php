<?php

declare(strict_types=1);

namespace App\Doctrine;

use Doctrine\Bundle\DoctrineBundle\Attribute\AsMiddleware;
use Doctrine\DBAL\Driver;
use Doctrine\DBAL\Driver\Connection;
use Doctrine\DBAL\Driver\Middleware;
use Doctrine\DBAL\Driver\Middleware\AbstractDriverMiddleware;

/**
 * Applies the per-connection SQLite settings
 * (journal_mode=WAL is persistent, it's set in the database file).
 */
#[AsMiddleware]
final readonly class SqlitePragmasMiddleware implements Middleware
{
    public const array PRAGMAS = [
        'synchronous' => 'NORMAL', // durable at commit in WAL mode (challenge rule 6)
        'busy_timeout' => '5000',  // wait for the write lock instead of failing with SQLITE_BUSY
    ];

    public function wrap(Driver $driver): Driver
    {
        return new class($driver) extends AbstractDriverMiddleware {
            public function connect(#[\SensitiveParameter] array $params): Connection
            {
                $connection = parent::connect($params);
                foreach (SqlitePragmasMiddleware::PRAGMAS as $pragma => $value) {
                    $connection->exec("PRAGMA {$pragma} = {$value}");
                }

                return $connection;
            }
        };
    }
}
