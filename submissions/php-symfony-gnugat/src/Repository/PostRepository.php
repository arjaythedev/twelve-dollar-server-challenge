<?php

declare(strict_types=1);

namespace App\Repository;

use App\Controller\CreatePostPayload;
use App\ReadModel\PostView;
use App\Security\JwtUser;
use Doctrine\DBAL\Connection;

/**
 * Plain SQL through DBAL: SQLite writes the response JSON of the reads, and the write is one INSERT.
 */
final readonly class PostRepository
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

    public function __construct(
        private Connection $connection,
    ) {
    }

    /**
     * The response of GET /feed: {"posts":[...]}.
     *
     * json() turns the subquery's text back into JSON, so json_group_array() doesn't quote it as a string.
     */
    public function findFeedJson(int $limit): string
    {
        $json = $this->connection->fetchOne(
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
        $json = $this->connection->fetchOne(
            "SELECT json_object('post', json(post)) FROM (".self::POST_SQL.' WHERE p.id = ?)',
            [$id],
        );

        return \is_string($json) ? $json : null;
    }

    public function exists(int $id): bool
    {
        return false !== $this->connection->fetchOne(
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
        $rows = $this->connection->fetchAllAssociative(
            'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at',
            [
                $author->id,
                $payload->body,
            ],
        );
        $id = $rows[0]['id'] ?? null;
        $createdAt = $rows[0]['created_at'] ?? null;
        if (!\is_int($id) || !\is_string($createdAt)) {
            throw new \LogicException('INSERT ... RETURNING returns the new row');
        }

        return new PostView(
            $id,
            $payload->body,
            $createdAt,
            $author->username,
            0,
        );
    }
}
