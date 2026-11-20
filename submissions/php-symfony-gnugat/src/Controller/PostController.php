<?php

declare(strict_types=1);

namespace App\Controller;

use App\Api\LikeOutput;
use App\Api\PostOutput;
use App\Repository\PostLikeRepository;
use App\Repository\PostRepository;
use App\Security\JwtUser;
use Symfony\Bundle\FrameworkBundle\Controller\AbstractController;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Attribute\ValueResolver;
use Symfony\Component\HttpKernel\Exception\NotFoundHttpException;
use Symfony\Component\JsonStreamer\JsonStreamWriter;
use Symfony\Component\Routing\Attribute\Route;
use Symfony\Component\Security\Http\Attribute\CurrentUser;
use Symfony\Component\TypeInfo\Type;

final class PostController extends AbstractController
{
    private const int FEED_SIZE = 20;

    public function __construct(
        private readonly PostRepository $posts,
        private readonly PostLikeRepository $likes,
        private readonly JsonStreamWriter $jsonStreamWriter,
    ) {
    }

    #[Route('/feed', methods: ['GET'])]
    public function feed(): JsonResponse
    {
        return new JsonResponse(
            $this->posts->findFeedJson(self::FEED_SIZE),
            json: true,
        );
    }

    #[Route('/posts/{id}', methods: ['GET'])]
    public function show(#[ValueResolver(PostId::class)] int $id): JsonResponse
    {
        return new JsonResponse(
            $this->posts->findPostJson($id)
                ?? throw new NotFoundHttpException('post not found'),
            json: true,
        );
    }

    #[Route('/posts', methods: ['POST'])]
    public function create(
        #[CurrentUser]
        JwtUser $user,
        Request $request,
    ): JsonResponse {
        return $this->writeJson(
            new PostOutput($this->posts->create($user, CreatePostPayload::fromRequest($request))),
            Response::HTTP_CREATED,
        );
    }

    #[Route('/posts/{id}/like', methods: ['POST'])]
    public function like(
        #[CurrentUser]
        JwtUser $user,
        #[ValueResolver(PostId::class)]
        int $id,
    ): JsonResponse {
        $added = $this->likes->add($user->id, $id);
        if (!$added && !$this->posts->exists($id)) {
            throw new NotFoundHttpException('post not found');
        }

        return $this->writeJson(
            new LikeOutput(
                true,
                !$added,
                $id,
            ),
            $added ? Response::HTTP_CREATED : Response::HTTP_OK,
        );
    }

    /**
     * For the writes (the reads get their JSON from SQLite): JsonStreamer writes the JSON with PHP code generated for
     * each output class at cache warmup.
     */
    private function writeJson(object $output, int $status = Response::HTTP_OK): JsonResponse
    {
        $json = $this->jsonStreamWriter->write($output, Type::object($output::class));

        return new JsonResponse((string) $json, $status, json: true);
    }
}
