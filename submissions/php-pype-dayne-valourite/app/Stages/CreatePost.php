<?php

declare(strict_types=1);

namespace App\Stages;

use App\Auth\TokenUser;
use App\Data\Post;
use App\Repositories\Posts;
use Pype\Http\Response;
use Pype\Validation\ValidatedInput;

final class CreatePost
{
    public function __invoke(TokenUser $user, ValidatedInput $input, Posts $posts): Response
    {
        $body = $input->get('body');
        [$id, $createdAt] = $posts->create($user->id, $body);

        return Response::make(['post' => new Post($id, $body, $createdAt, $user->username, 0)], 201);
    }
}
