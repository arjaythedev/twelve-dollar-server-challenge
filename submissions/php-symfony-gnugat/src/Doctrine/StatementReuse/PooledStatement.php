<?php

declare(strict_types=1);

namespace App\Doctrine\StatementReuse;

use Doctrine\DBAL\Driver\Middleware\AbstractStatementMiddleware;
use Doctrine\DBAL\Driver\Result;
use Doctrine\DBAL\Driver\Statement;

/**
 * A statement checked out of the pool: it goes back when this object is destroyed (no caller, no result left).
 */
final class PooledStatement extends AbstractStatementMiddleware
{
    private bool $reusable = true;

    /**
     * @param \Closure(Statement): void $release
     */
    public function __construct(
        private readonly Statement $statement,
        private readonly \Closure $release,
    ) {
        parent::__construct($statement);
    }

    public function execute(): Result
    {
        try {
            return new PooledResult(parent::execute(), $this);
        } catch (\Throwable $throwable) {
            $this->reusable = false;

            throw $throwable;
        }
    }

    public function __destruct()
    {
        if ($this->reusable) {
            ($this->release)($this->statement);
        }
    }
}
