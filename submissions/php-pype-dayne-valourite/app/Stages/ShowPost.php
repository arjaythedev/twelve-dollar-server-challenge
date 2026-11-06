<?php

declare(strict_types=1);

namespace App\Stages;

use App\Repositories\Posts;
use Pype\Exceptions\HttpException;

final class ShowPost
{
    public function __invoke(int $id, Posts $posts): array
    {
        return ['post' => $posts->find($id) ?? throw HttpException::notFound('post not found')];
    }
}
