<?php

declare(strict_types=1);

namespace App\Providers;

use App\Outlets\ApiOutlet;
use Pype\Application;
use Pype\Provider;
use Pype\Stages\JsonBody;
use Pype\Stages\TrimStrings;

final class AppProvider implements Provider
{
    public function register(Application $app): void
    {
        $app->through(new JsonBody(maxBytes: 16 * 1024), new TrimStrings());
        $app->outlet(ApiOutlet::class);
    }
}
