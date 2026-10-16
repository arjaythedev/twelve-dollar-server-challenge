<?php

declare(strict_types=1);

use App\Security\AuthEntryPoint;
use App\Security\AuthFailureHandler;
use App\Security\JwtTokenHandler;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;

return static function (ContainerConfigurator $container): void {
    $container->extension('security', [
        'firewalls' => [
            'api' => [
                'pattern' => '^/posts(/|$)',
                'methods' => ['POST'],
                'stateless' => true,
                'entry_point' => AuthEntryPoint::class,
                'access_token' => [
                    'token_handler' => JwtTokenHandler::class,
                    'failure_handler' => AuthFailureHandler::class,
                ],
            ],
        ],
        'access_control' => [
            ['path' => '^/posts(/|$)', 'methods' => ['POST'], 'roles' => 'IS_AUTHENTICATED'],
        ],
    ]);
};
