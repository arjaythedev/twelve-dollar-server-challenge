<?php

declare(strict_types=1);

/**
 * For phpstan/phpstan-doctrine: the entity metadata lets it check DQL and infer query result types.
 * Booting the kernel creates the connection service, which needs the env var, but never connects.
 */

use App\Kernel;
use Symfony\Component\Dotenv\Dotenv;

require __DIR__.'/../../vendor/autoload.php';

$_SERVER['SQLITE_PATH'] ??= '/nonexistent/feed.db';
new Dotenv()->bootEnv(__DIR__.'/../../.env');

$kernel = new Kernel($_SERVER['APP_ENV'], (bool) $_SERVER['APP_DEBUG']);
$kernel->boot();

return $kernel->getContainer()->get('doctrine')->getManager();
