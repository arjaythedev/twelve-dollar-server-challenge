<?php

declare(strict_types=1);

namespace App\Controller;

use App\Api\LikeOutput;
use App\Api\PostOutput;
use App\Entity\Post;
use App\Entity\PostLike;
use App\Entity\User;
use App\ReadModel\PostView;
use App\Repository\PostLikeRepository;
use App\Repository\PostRepository;
use App\Security\JwtUser;
use Doctrine\DBAL\Exception\UniqueConstraintViolationException;
use Doctrine\ORM\EntityManagerInterface;
use Symfony\Bundle\FrameworkBundle\Controller\AbstractController;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Attribute\MapRequestPayload;
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
        private readonly EntityManagerInterface $entityManager,
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
        #[MapRequestPayload(validationFailedStatusCode: Response::HTTP_BAD_REQUEST)]
        CreatePostPayload $payload,
    ): JsonResponse {
        $author = $this->entityManager->getReference(User::class, $user->id)
            ?? throw new \LogicException('user reference not available');

        $post = Post::fromPayload($author, $payload);
        $this->entityManager->persist($post);
        $this->entityManager->flush();

        return $this->writeJson(
            new PostOutput(PostView::fromPost($post, $user->username)),
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
        $post = $this->posts->find($id)
            ?? throw new NotFoundHttpException('post not found');
        $liker = $this->entityManager->getReference(User::class, $user->id)
            ?? throw new \LogicException('user reference not available');

        $alreadyLiked = null !== $this->likes->findOneBy([
            'user' => $liker,
            'post' => $post,
        ]);
        if (!$alreadyLiked) {
            try {
                $this->entityManager->persist(new PostLike(
                    $liker,
                    $post,
                ));
                $this->entityManager->flush();
            } catch (UniqueConstraintViolationException) {
                $alreadyLiked = true; // liked by a concurrent request
            }
        }

        return $this->writeJson(
            new LikeOutput(
                true,
                $alreadyLiked,
                $id,
            ),
            $alreadyLiked ? Response::HTTP_OK : Response::HTTP_CREATED,
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
