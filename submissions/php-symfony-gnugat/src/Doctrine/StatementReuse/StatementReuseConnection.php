<?php

declare(strict_types=1);

namespace App\Doctrine\StatementReuse;

use Doctrine\DBAL\Driver\Connection;
use Doctrine\DBAL\Driver\Middleware\AbstractConnectionMiddleware;
use Doctrine\DBAL\Driver\Statement;

/**
 * A pool of idle prepared statements, per SQL string, owned by one driver connection.
 *
 * - prepare() checks a statement out of the pool (or prepares a new one): two users of the same SQL at the same
 *   time (an open cursor, a statement executed again before its previous result is freed) never share one.
 * - A statement goes back to the pool when its PooledStatement is destroyed, which can't happen while one of its
 *   results is alive (results hold a reference to it). By then its cursor is reset (see PooledResult), so pooled
 *   statements never hold a read snapshot open, or block a COMMIT.
 * - A statement whose execution failed is discarded.
 * - The pool lives and dies with this driver connection: when DBAL closes or replaces it, nothing is reused.
 */
final class StatementReuseConnection extends AbstractConnectionMiddleware
{
    private const int MAX_SQL_STRINGS = 128;

    private const int MAX_IDLE_PER_SQL = 2;

    /** @var array<string, list<Statement>> */
    private array $idle = [];

    public function prepare(string $sql): Statement
    {
        $statement = [] !== ($this->idle[$sql] ?? []) ? array_pop($this->idle[$sql]) : parent::prepare($sql);

        return new PooledStatement($statement, function (Statement $statement) use ($sql): void {
            $this->release($sql, $statement);
        });
    }

    private function release(string $sql, Statement $statement): void
    {
        $idle = $this->idle[$sql] ?? null;
        if (null === $idle && \count($this->idle) >= self::MAX_SQL_STRINGS) {
            return;
        }

        if (\count($idle ?? []) < self::MAX_IDLE_PER_SQL) {
            $this->idle[$sql][] = $statement;
        }
    }
}
