<?php

declare(strict_types=1);

use Pype\Application;
use Pype\Support\Env;

require __DIR__ . '/../vendor/autoload.php';

Env::load(__DIR__ . '/../.env');

$app = new Application(
    basePath: dirname(__DIR__),
    config: Application::loadConfig(__DIR__ . '/../config'),
);

$app->routes(__DIR__ . '/../routes/api.php');

return $app;
