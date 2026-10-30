<?php

declare(strict_types=1);

namespace App\Repository;

use App\Controller\CreatePostPayload;
use App\Entity\Post;
use App\ReadModel\PostView;
use App\Security\JwtUser;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
use Doctrine\Persistence\ManagerRegistry;

/**
 * Plain SQL through DBAL: the reads are mapped straight to PostView, and the write is one INSERT.
 *
 * @extends ServiceEntityRepository<Post>
 *
 * @phpstan-import-type PostRow from PostView
 */
class PostRepository extends ServiceEntityRepository
{
    /**
     * One row per post, with its author and its like count as of now.
     *
     * Counting likes.post_id (rather than a non-key column) lets SQLite answer from likes_post_id_idx alone.
     */
    private const string VIEW_SQL = <<<'SQL'
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT COUNT(l.post_id) FROM likes l WHERE l.post_id = p.id) AS like_count
      FROM posts p
      JOIN users u ON u.id = p.user_id
    SQL;

    public function __construct(ManagerRegistry $registry)
    {
        parent::__construct($registry, Post::class);
    }

    /**
     * @return list<PostView>
     */
    public function findLatestViews(int $limit): array
    {
        /** @var list<PostRow> $rows */
        $rows = $this->getEntityManager()->getConnection()->fetchAllAssociative(
            self::VIEW_SQL.' ORDER BY p.created_at DESC, p.id DESC LIMIT ?',
            [$limit],
        );

        return array_map(PostView::fromRow(...), $rows);
    }

    public function findView(int $id): ?PostView
    {
        /** @var PostRow|false $row */
        $row = $this->getEntityManager()->getConnection()->fetchAssociative(
            self::VIEW_SQL.' WHERE p.id = ?',
            [$id],
        );

        return false === $row ? null : PostView::fromRow($row);
    }

    public function exists(int $id): bool
    {
        return false !== $this->getEntityManager()->getConnection()->fetchOne(
            'SELECT 1 FROM posts WHERE id = ?',
            [$id],
        );
    }

    /**
     * Inserts the post of POST /posts. The author's username comes from the token, and a new post has no likes.
     *
     * One autocommitted statement: reading all the rows of RETURNING runs it to completion, which commits it, so the
     * 201 is only sent once the post is committed.
     */
    public function create(JwtUser $author, CreatePostPayload $payload): PostView
    {
        if (!\is_string($payload->body)) {
            throw new \LogicException('body must be validated first');
        }

        $body = trim($payload->body);

        $rows = $this->getEntityManager()->getConnection()->fetchAllAssociative(
            'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at',
            [
                $author->id,
                $body,
            ],
        );
        $id = $rows[0]['id'] ?? null;
        $createdAt = $rows[0]['created_at'] ?? null;
        if (!\is_int($id) || !\is_string($createdAt)) {
            throw new \LogicException('INSERT ... RETURNING returns the new row');
        }

        return new PostView(
            $id,
            $body,
            $createdAt,
            $author->username,
            0,
        );
    }
}
