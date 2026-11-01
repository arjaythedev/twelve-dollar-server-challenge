<?php

declare(strict_types=1);

use Rector\Caching\ValueObject\Storage\FileCacheStorage;
use Rector\CodeQuality\Rector\Assign\CombinedAssignRector;
use Rector\Config\RectorConfig;
use Rector\DeadCode\Rector\ClassMethod\RemoveUnusedPromotedPropertyRector;
use Rector\Naming\Rector\Assign\RenameVariableToMatchMethodCallReturnTypeRector;
use Rector\Naming\Rector\Class_\RenamePropertyToMatchTypeRector;
use Rector\Naming\Rector\ClassMethod\RenameParamToMatchTypeRector;
use Rector\Set\ValueObject\SetList;
use Rector\Visibility\Rector\ClassConst\ChangeConstantVisibilityRector;
use Rector\Visibility\Rector\ClassMethod\ChangeMethodVisibilityRector;

return RectorConfig::configure()
    ->withCache(
        cacheDirectory: __DIR__.'/var/rector',
        cacheClass: FileCacheStorage::class,
    )
    ->withPaths([
        __DIR__,
    ])
    ->withSkip([
        // —— Excluded paths ———————————————————————————————————————————————————
        // Excluded folders
        // [php-symfony-gnugat]
        __DIR__.'/config/bundles.php',
        __DIR__.'/config/reference.php',
        __DIR__.'/var',
        __DIR__.'/vendor',

        // —— Excluded rules ———————————————————————————————————————————————————
        // [CODE_QUALITY]
        CombinedAssignRector::class,
        // [NAMING]
        RenameParamToMatchTypeRector::class,
        RenamePropertyToMatchTypeRector::class,
        RenameVariableToMatchMethodCallReturnTypeRector::class,
        // [DEAD_CODE]
        // Never read on purpose: holding the statement keeps it out of the pool while the result is alive
        RemoveUnusedPromotedPropertyRector::class => [
            __DIR__.'/src/Doctrine/StatementReuse/PooledResult.php',
        ],
    ])
    // —— PHP ——————————————————————————————————————————————————————————————————
    // Version picked from composer.json's `require.php`
    ->withPhpSets()
    ->withSets([
        // —— Core —————————————————————————————————————————————————————————————
        SetList::CODE_QUALITY,
        SetList::CODING_STYLE,
        SetList::DEAD_CODE,
        SetList::EARLY_RETURN,
        SetList::INSTANCEOF,
        SetList::NAMING,
        SetList::PRIVATIZATION,
        SetList::RECTOR_PRESET,
        SetList::TYPE_DECLARATION,
        SetList::TYPE_DECLARATION_DOCBLOCKS,
    ])
    ->withRules([
        // —— Core —————————————————————————————————————————————————————————————
        // Inherit parent visibility
        ChangeConstantVisibilityRector::class,
        ChangeMethodVisibilityRector::class,
    ]);
