<?php

declare(strict_types=1);

namespace App;

use Symfony\Bundle\FrameworkBundle\Kernel\MicroKernelTrait;
use Symfony\Component\HttpKernel\Kernel as BaseKernel;

final class Kernel extends BaseKernel
{
    use MicroKernelTrait;

    public static int $startedAt = 0;

    public function __construct(string $environment, bool $debug)
    {
        self::$startedAt = self::$startedAt ?: time();
        parent::__construct($environment, $debug);
    }
}
