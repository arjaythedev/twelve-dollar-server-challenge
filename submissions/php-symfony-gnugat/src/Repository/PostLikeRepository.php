<?php

declare(strict_types=1);

namespace App\Repository;

use App\Entity\PostLike;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
use Doctrine\Persistence\ManagerRegistry;

/**
 * @extends ServiceEntityRepository<PostLike>
 */
class PostLikeRepository extends ServiceEntityRepository
{
    public function __construct(ManagerRegistry $registry)
    {
        parent::__construct($registry, PostLike::class);
    }

    /**
     * The user likes the post: true when the like was added, false when they had already liked it or there is no
     * such post.
     *
     * One autocommitted statement: the like is committed when it returns, and two concurrent likes by the same user
     * can't both add one (ON CONFLICT on the primary key).
     */
    public function add(int $userId, int $postId): bool
    {
        return 1 === $this->getEntityManager()->getConnection()->executeStatement(
            'INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING',
            [
                $userId,
                $postId,
            ],
        );
    }
}
