<?php

declare(strict_types=1);

use App\Kernel;
use Symfony\Component\HttpFoundation\Request;

require dirname(__DIR__).'/vendor/autoload.php';

$kernel = new Kernel($_SERVER['APP_ENV'] ?? 'prod', (bool) ($_SERVER['APP_DEBUG'] ?? false));

// Classic mode, just in case...
if (!\function_exists('frankenphp_handle_request')) {
    $request = Request::createFromGlobals();
    $response = $kernel->handle($request);
    $response->send();
    $kernel->terminate($request, $response);

    return;
}

$kernel->boot();

$handler = static function () use ($kernel): void {
    $request = Request::createFromGlobals();
    $response = $kernel->handle($request);
    $response->send();
    $kernel->terminate($request, $response);
};

$maxRequests = (int) ($_SERVER['MAX_REQUESTS'] ?? 0);
for ($served = 0; 0 === $maxRequests || $served < $maxRequests; ++$served) {
    $keepRunning = frankenphp_handle_request($handler);
    gc_collect_cycles();
    if (!$keepRunning) {
        break;
    }
}

$kernel->shutdown();
