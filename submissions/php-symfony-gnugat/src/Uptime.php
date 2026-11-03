<?php

declare(strict_types=1);

namespace App;

use Symfony\Component\DependencyInjection\Attribute\Autowire;

/**
 * Counts from when start.sh launched the server: the container is rebuilt on every request in classic mode
 * (and every 500 requests in worker mode), so the start time can't come from the container itself.
 */
final readonly class Uptime
{
    public function __construct(
        #[Autowire('%env(int:APP_STARTED_AT)%')]
        private int $startedAt,
    ) {
    }

    public function seconds(): int
    {
        return time() - $this->startedAt;
    }
}
