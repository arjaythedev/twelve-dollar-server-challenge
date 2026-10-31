<?php

declare(strict_types=1);

use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;

return static function (ContainerConfigurator $configurator): void {
    $services = $configurator->services()
        ->defaults()
            ->autowire()
            ->autoconfigure();

    $services->load('App\\', '../src/')
        ->exclude([
            '../src/Kernel.php',
            '../src/Http/',
            '../src/Security/JwtUser.php',
        ]);
};
