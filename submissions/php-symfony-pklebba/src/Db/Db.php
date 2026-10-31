<?php

declare(strict_types=1);

namespace App\Db;

use Doctrine\DBAL\Connection;
use Doctrine\DBAL\DriverManager;
use Doctrine\DBAL\Statement;
use Symfony\Component\DependencyInjection\Attribute\Autowire;

/**
 * One SQLite connection per PHP worker thread, opened on first use and kept for the life of the
 * worker.
 */
final class Db
{
    private ?Connection $connection = null;

    /** @var array<string, Statement> */
    private array $statements = [];

    public function __construct(
        #[Autowire(env: 'SQLITE_PATH')]
        private readonly string $path,
    ) {
    }

    public function connection(): Connection
    {
        if (null === $this->connection) {
            $connection = DriverManager::getConnection([
                'driver' => 'pdo_sqlite',
                'path' => $this->path,
                'driverOptions' => [\PDO::ATTR_TIMEOUT => 10],
            ]);
            foreach ([
                'journal_mode = WAL',
                'synchronous = NORMAL',
                'busy_timeout = 10000',
                'mmap_size = 1073741824',
                'cache_size = -65536',
                'temp_store = MEMORY',
            ] as $pragma) {
                $connection->executeQuery('PRAGMA '.$pragma)->free();
            }
            $this->connection = $connection;
        }

        return $this->connection;
    }

    public function statement(string $sql): Statement
    {
        return $this->statements[$sql] ??= $this->connection()->prepare($sql);
    }
}
