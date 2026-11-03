<?php

declare(strict_types=1);

namespace App\Doctrine\StatementReuse;

use Doctrine\DBAL\Driver\Middleware\AbstractResultMiddleware;
use Doctrine\DBAL\Driver\Result;

/**
 * Resets the statement's cursor (PDO closeCursor(), sqlite3_reset()) as soon as the rows are consumed, on free(),
 * and at the latest when the result is destroyed: an unreset SQLite statement keeps its read snapshot open.
 *
 * Holds its PooledStatement, so the statement can't go back to the pool while this result is alive.
 */
final class PooledResult extends AbstractResultMiddleware
{
    private bool $freed = false;

    public function __construct(
        private readonly Result $result,
        /** @phpstan-ignore property.onlyWritten (holding it keeps the statement checked out while this result is alive) */
        private readonly PooledStatement $statement,
    ) {
        parent::__construct($result);
    }

    public function fetchNumeric(): array|false
    {
        return $this->freeWhenDone(parent::fetchNumeric());
    }

    public function fetchAssociative(): array|false
    {
        return $this->freeWhenDone(parent::fetchAssociative());
    }

    public function fetchOne(): mixed
    {
        return $this->freeWhenDone(parent::fetchOne());
    }

    public function fetchAllNumeric(): array
    {
        return $this->freeAfter(parent::fetchAllNumeric());
    }

    public function fetchAllAssociative(): array
    {
        return $this->freeAfter(parent::fetchAllAssociative());
    }

    public function fetchFirstColumn(): array
    {
        return $this->freeAfter(parent::fetchFirstColumn());
    }

    public function free(): void
    {
        if (!$this->freed) {
            $this->freed = true;
            $this->result->free();
        }
    }

    public function __destruct()
    {
        $this->free();
    }

    /**
     * @template T
     *
     * @param T $row
     *
     * @return T
     */
    private function freeWhenDone(mixed $row): mixed
    {
        if (false === $row) {
            $this->free();
        }

        return $row;
    }

    /**
     * @template T
     *
     * @param T $rows
     *
     * @return T
     */
    private function freeAfter(mixed $rows): mixed
    {
        $this->free();

        return $rows;
    }
}
