<?php

use PhpCsFixer\Runner\Parallel\ParallelConfigFactory;

/**
 * PHP CS Fixer documentation:
 * - Homepage: https://cs.symfony.com/
 * - List of all available rules: https://cs.symfony.com/doc/rules/index.html
 * - List of all available rule sets: https://cs.symfony.com/doc/ruleSets/index.html
 * - Find / Compare / See History rules: https://mlocati.github.io/php-cs-fixer-configurator
 *
 * To inspect a specific rule (e.g. `blank_line_before_statement`), run:
 *
 * ```console
 * > php-cs-fixer describe blank_line_before_statement
 * ```
 *
 * ------------------------------------------------------------------------------
 *
 * `new \PhpCsFixer\Finder()` is equivalent to:
 *
 * ```php
 * \Symfony\Component\Finder\Finder::create()
 *     ->files()
 *     ->name('/\.php$/')
 *     ->exclude('vendor')
 *     ->ignoreVCSIgnored(true) // Follow rules establish in .gitignore
 *     ->ignoreDotFiles(false) // Do not ignore files starting with `.`, like `.php-cs-fixer-dist.php`
 * ;
 * ```
 */

$finder = (new PhpCsFixer\Finder())
    ->in([
        __DIR__,
    ])
    ->exclude([
        'var',
    ])
    ->notPath([
        // Note: `notPath()` expect paths relatives to the ones provided in `in()`
        'config/bundles.php',
        'config/reference.php',
    ])
;

return (new PhpCsFixer\Config())
    ->setRules([
        // —— CS Rule Sets —————————————————————————————————————————————————————
        '@Symfony' => true,
        '@Symfony:risky' => true,
        '@PHP8x5Migration' => true,
        '@PHP8x5Migration:risky' => true,

        // —— Overriden rules ——————————————————————————————————————————————————

        // [Symfony] defaults without `['elements']['parameters']`, adding it here
        'trailing_comma_in_multiline' => [
            'after_heredoc' => true,
            'elements' => [
                'arguments',
                'array_destructuring',
                'arrays',
                'match',
                'parameters',
            ],
        ],

        // [Symfony] defaults to `true`, allowing multiline throws here
        'single_line_throw' => false,

        // [Symfony] defaults to `true`, allowing `=>` on a different line here
        'no_multiline_whitespace_around_double_arrow' => false,

        // [Symfony] defaults to allowing FQCNs in code, use `use` statements instead here
        'fully_qualified_strict_types' => ['import_symbols' => true],

        // [PHP8x5Migration] defaults to `start_plus_one`, using `same_as_start` here
        'heredoc_indentation' => ['indentation' => 'same_as_start'],

        // —— Disabed rules due to breaking changes ————————————————————————————

        // —— Additional rules —————————————————————————————————————————————————

        // [PhpCsFixer]
        'heredoc_to_nowdoc' => true,
    ])
    ->setRiskyAllowed(true)
    ->setParallelConfig(ParallelConfigFactory::detect())
    ->setUsingCache(true)
    ->setFinder($finder)
;
