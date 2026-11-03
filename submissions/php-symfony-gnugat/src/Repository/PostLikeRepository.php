<?php

declare(strict_types=1);

namespace App\Repository;

use Doctrine\DBAL\Connection;

final readonly class PostLikeRepository
{
    public function __construct(
        private Connection $connection,
    ) {
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
        return 1 === $this->connection->executeStatement(
            'INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING',
            [
                $userId,
                $postId,
            ],
        );
    }
}
