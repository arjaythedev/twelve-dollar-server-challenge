<?php

declare(strict_types=1);

use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;

return static function (ContainerConfigurator $container): void {
    $container->extension('framework', [
        'secret' => 'twelve-dollar-server-challenge',
        'http_method_override' => false,
        'handle_all_throwables' => true,
        'php_errors' => ['log' => true],
        'router' => ['strict_requirements' => null],
    ]);
};
