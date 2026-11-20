<?php

declare(strict_types=1);

use App\Stages\CreatePost;
use App\Stages\LikePost;
use App\Stages\ShowFeed;
use App\Stages\ShowPost;
use App\Stages\ValidatePost;
use App\Stages\ValidPostId;
use Pype\Database\Connection;
use Pype\Http\Response;
use Pype\Routing\Router;

/** @var Router $router */

$started = time();   // the app boots once per OpenSwoole server

$router->get('/health', function (Connection $db) use ($started) {
    try {
        $db->scalar('SELECT 1');
    } catch (Throwable $e) {
        return Response::make(['status' => 'degraded', 'db' => 'unreachable', 'error' => $e->getMessage()], 503);
    }

    return ['status' => 'ok', 'db' => 'ok', 'uptime_s' => time() - $started];
});

$router->get('/feed', ShowFeed::class);
$router->get('/posts/{id}', [ValidPostId::class, ShowPost::class]);

// Auth first, then the id (the spec checks them in that order).
$router->group('/posts', ['auth'], function (Router $router) {
    $router->post('/', [ValidatePost::class, CreatePost::class]);
    $router->post('/{id}/like', [ValidPostId::class, LikePost::class]);
});

