<?php

declare(strict_types=1);

namespace App\Repository;

use App\Entity\Post;
use App\ReadModel\PostView;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
use Doctrine\Persistence\ManagerRegistry;

/**
 * Writes go through the ORM (Post entities); reads are a read model: plain SQL, mapped straight to PostView.
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
}
