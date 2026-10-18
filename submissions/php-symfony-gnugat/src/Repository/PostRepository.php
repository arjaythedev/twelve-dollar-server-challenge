<?php

declare(strict_types=1);

namespace App\Repository;

use App\Entity\Post;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
use Doctrine\Persistence\ManagerRegistry;

/**
 * Writes go through the ORM (Post entities); reads are a read model: plain SQL, where SQLite writes the response JSON.
 *
 * @extends ServiceEntityRepository<Post>
 */
class PostRepository extends ServiceEntityRepository
{
    /**
     * One row per post: the post object of the API (same keys, in the same order, as PostView), with its author and
     * its like count as of now.
     *
     * Counting likes.post_id (rather than a non-key column) lets SQLite answer from likes_post_id_idx alone.
     */
    private const string POST_SQL = <<<'SQL'
    SELECT p.id, p.created_at,
           json_object(
               'id', p.id,
               'body', p.body,
               'created_at', p.created_at,
               'author', u.username,
               'like_count', (SELECT COUNT(l.post_id) FROM likes l WHERE l.post_id = p.id)
           ) AS post
      FROM posts p
      JOIN users u ON u.id = p.user_id
    SQL;

    public function __construct(ManagerRegistry $registry)
    {
        parent::__construct($registry, Post::class);
    }

    /**
     * The response of GET /feed: {"posts":[...]}.
     *
     * json() turns the subquery's text back into JSON, so json_group_array() doesn't quote it as a string.
     */
    public function findFeedJson(int $limit): string
    {
        $json = $this->getEntityManager()->getConnection()->fetchOne(
            "SELECT json_object('posts', json_group_array(json(post) ORDER BY created_at DESC, id DESC))"
            .' FROM ('.self::POST_SQL.' ORDER BY p.created_at DESC, p.id DESC LIMIT ?)',
            [$limit],
        );
        if (!\is_string($json)) {
            throw new \LogicException('an aggregate query returns one row');
        }

        return $json;
    }

    /**
     * The response of GET /posts/{id}: {"post":{...}}, or null when there is no such post.
     */
    public function findPostJson(int $id): ?string
    {
        $json = $this->getEntityManager()->getConnection()->fetchOne(
            "SELECT json_object('post', json(post)) FROM (".self::POST_SQL.' WHERE p.id = ?)',
            [$id],
        );

        return \is_string($json) ? $json : null;
    }
}
