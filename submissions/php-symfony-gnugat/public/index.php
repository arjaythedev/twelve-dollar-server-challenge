<?php

declare(strict_types=1);

use App\Kernel;
use Symfony\Component\HttpFoundation\Request;

require dirname(__DIR__).'/vendor/autoload.php';

// The variables of .env.local.php (composer dump-env prod). FrankenPHP gives each request a new $_SERVER, so they are
// merged into every request's: symfony/runtime merges the whole $_SERVER the worker started with.
$env = require dirname(__DIR__).'/.env.local.php';
$_SERVER += $env;
$kernel = new Kernel($_SERVER['APP_ENV'], (bool) ($_SERVER['APP_DEBUG'] ?? 'prod' !== $_SERVER['APP_ENV']));

// Classic mode (local-bench/Caddyfile.classic): one request per script. FrankenPHP sets FRANKENPHP_WORKER for workers.
if (!isset($_SERVER['FRANKENPHP_WORKER'])) {
    $request = Request::createFromGlobals();
    $response = $kernel->handle($request);
    $response->send();
    $kernel->terminate($request, $response);

    return;
}

// Worker mode: Symfony boots once, then handles requests in a loop, restarted every 500 requests like symfony/runtime
$kernel->boot();
$request = $response = null;
$handler = static function () use ($kernel, $env, &$request, &$response): void {
    $_SERVER += $env;
    $request = Request::createFromGlobals();
    $response = $kernel->handle($request);
    $response->send();
};
$loops = 0;
do {
    $keepRunning = frankenphp_handle_request($handler);
    // After the response is sent: Symfony resets its services
    if (null !== $request && null !== $response) {
        $kernel->terminate($request, $response);
    }
    gc_collect_cycles();
} while ($keepRunning && ++$loops < 500);
