<?php

declare(strict_types=1);

namespace App\Stages;

use App\Repositories\Posts;

final class ShowFeed
{
    public function __invoke(Posts $posts): array
    {
        return ['posts' => $posts->newest(20)];
    }
}
