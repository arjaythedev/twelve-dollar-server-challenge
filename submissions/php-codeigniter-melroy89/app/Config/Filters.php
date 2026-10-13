<?php
namespace Config;

class Filters extends \CodeIgniter\Config\Filters
{
    public array $aliases = [];
    public array $required = ['before' => [], 'after' => []];
    public array $globals = ['before' => [], 'after' => []];
    public array $methods = [];
    public array $filters = [];
}
