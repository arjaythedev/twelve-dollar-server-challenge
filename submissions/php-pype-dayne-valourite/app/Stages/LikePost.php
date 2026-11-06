<?php

declare(strict_types=1);

namespace App\Stages;

use App\Auth\TokenUser;
use App\Repositories\Posts;
use Pype\Exceptions\HttpException;
use Pype\Http\Response;

final class LikePost
{
    public function __invoke(int $id, TokenUser $user, Posts $posts): Response
    {
        if (!$posts->exists($id)) {
            throw HttpException::notFound('post not found');
        }

        $new = $posts->like($user->id, $id);

        return Response::make(['liked' => true, 'already_liked' => !$new, 'post_id' => $id], $new ? 201 : 200);
    }
}
