<?php

declare(strict_types=1);

namespace App\Controller;

use App\Db\PostRepository;
use App\Http\Json;
use App\Security\JwtUser;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Attribute\AsController;
use Symfony\Component\Routing\Attribute\Route;
use Symfony\Component\Security\Http\Attribute\CurrentUser;

#[AsController]
final class PostController
{
    public function __construct(private readonly PostRepository $posts)
    {
    }

    #[Route('/feed', methods: ['GET'])]
    public function feed(): Response
    {
        return Json::ok(['posts' => $this->posts->feed()]);
    }

    #[Route('/posts/{id}', methods: ['GET'], requirements: ['id' => '[^/]+'])]
    public function show(string $id): Response
    {
        if (null === $postId = self::postId($id)) {
            return Json::error(Response::HTTP_BAD_REQUEST, 'invalid post id');
        }
        if (null === $post = $this->posts->find($postId)) {
            return Json::error(Response::HTTP_NOT_FOUND, 'post not found');
        }

        return Json::ok(['post' => $post]);
    }

    #[Route('/posts', methods: ['POST'])]
    public function create(Request $request, #[CurrentUser] JwtUser $user): Response
    {
        try {
            $payload = json_decode($request->getContent(), true, 64, \JSON_THROW_ON_ERROR);
        } catch (\JsonException) {
            return Json::error(Response::HTTP_BAD_REQUEST, 'malformed JSON body');
        }
        $body = \is_array($payload) && \is_string($payload['body'] ?? null) ? trim($payload['body']) : '';
        if ('' === $body) {
            return Json::error(Response::HTTP_BAD_REQUEST, 'body is required');
        }
        if (mb_strlen($body) > 500) {
            return Json::error(Response::HTTP_BAD_REQUEST, 'body must be at most 500 characters');
        }

        $row = $this->posts->create($user->id, $body);

        return Json::ok(['post' => [
            'id' => $row['id'],
            'body' => $body,
            'created_at' => $row['created_at'],
            'author' => $user->username,
            'like_count' => 0,
        ]], Response::HTTP_CREATED);
    }

    #[Route('/posts/{id}/like', methods: ['POST'], requirements: ['id' => '[^/]+'])]
    public function like(string $id, #[CurrentUser] JwtUser $user): Response
    {
        if (null === $postId = self::postId($id)) {
            return Json::error(Response::HTTP_BAD_REQUEST, 'invalid post id');
        }
        if (1 === $this->posts->like($user->id, $postId)) {
            return Json::ok(['liked' => true, 'already_liked' => false, 'post_id' => $postId], Response::HTTP_CREATED);
        }
        if (!$this->posts->exists($postId)) {
            return Json::error(Response::HTTP_NOT_FOUND, 'post not found');
        }

        return Json::ok(['liked' => true, 'already_liked' => true, 'post_id' => $postId]);
    }

    /** A positive integer in decimal notation, nothing else (so "0", "-1", "abc" and "1.5" are rejected). */
    private static function postId(string $id): ?int
    {
        if (!preg_match('/^[1-9][0-9]{0,17}$/D', $id)) {
            return null;
        }

        return (int) $id;
    }
}
