<?php

declare(strict_types=1);

namespace App\Stages;

use Closure;
use Pype\Exceptions\HttpException;
use Pype\Http\Request;
use Pype\Pipeline\Stage;

/** 400 unless the {id} route parameter is a positive integer. */
final class ValidPostId implements Stage
{
    public function handle(Request $request, Closure $next): mixed
    {
        if (!preg_match('/^[1-9]\d*$/', (string) $request->param('id'))) {
            throw new HttpException(400, 'invalid post id');
        }

        return $next($request);
    }
}
