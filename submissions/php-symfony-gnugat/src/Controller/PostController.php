<?php

declare(strict_types=1);

namespace App\Controller;

use App\Entity\Post;
use App\Entity\PostLike;
use App\Entity\User;
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
use Symfony\Component\Routing\Attribute\Route;
use Symfony\Component\Security\Http\Attribute\CurrentUser;

final class PostController extends AbstractController
{
    private const int FEED_SIZE = 20;

    public function __construct(
        private readonly PostRepository $posts,
        private readonly PostLikeRepository $likes,
        private readonly EntityManagerInterface $entityManager,
    ) {
    }

    #[Route('/feed', methods: ['GET'])]
    public function feed(): JsonResponse
    {
        return $this->json([
            'posts' => $this->posts->findLatest(self::FEED_SIZE),
        ]);
    }

    #[Route('/posts/{id}', methods: ['GET'])]
    public function show(#[ValueResolver(PostId::class)] int $id): JsonResponse
    {
        return $this->json([
            'post' => $this->posts->find($id)
                ?? throw new NotFoundHttpException('post not found'),
        ]);
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

        return $this->json([
            'post' => $post,
        ], Response::HTTP_CREATED);
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

        return $this->json([
            'liked' => true,
            'already_liked' => $alreadyLiked,
            'post_id' => $id,
        ], $alreadyLiked ? Response::HTTP_OK : Response::HTTP_CREATED);
    }
}
