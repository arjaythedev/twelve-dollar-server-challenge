<?php

declare(strict_types=1);

namespace App\Doctrine;

use App\Doctrine\StatementReuse\StatementReuseConnection;
use Doctrine\Bundle\DoctrineBundle\Attribute\AsMiddleware;
use Doctrine\DBAL\Driver;
use Doctrine\DBAL\Driver\Connection;
use Doctrine\DBAL\Driver\Middleware;
use Doctrine\DBAL\Driver\Middleware\AbstractDriverMiddleware;

/**
 * Reuses prepared statements per SQL string for the life of the driver connection: in worker mode the connection
 * outlives requests, so each query is parsed and planned once instead of on every request.
 *
 * Statements cache the query plan, not data (challenge rule 5). See StatementReuseConnection for the safety rules.
 */
#[AsMiddleware]
final readonly class StatementReuseMiddleware implements Middleware
{
    public function wrap(Driver $driver): Driver
    {
        return new class($driver) extends AbstractDriverMiddleware {
            public function connect(#[\SensitiveParameter] array $params): Connection
            {
                return new StatementReuseConnection(parent::connect($params));
            }
        };
    }
}
