<?php

declare(strict_types=1);

namespace App\ReadModel;

use App\Entity\Post;
use Symfony\Component\JsonStreamer\Attribute\JsonStreamable;
use Symfony\Component\JsonStreamer\Attribute\StreamedName;

/**
 * The post object of the API: properties are written in this order.
 *
 * @phpstan-type PostRow array{id: int, body: string, created_at: string, username: string, like_count: int}
 */
#[JsonStreamable]
final readonly class PostView
{
    public function __construct(
        public int $id,
        public string $body,
        #[StreamedName('created_at')]
        public string $createdAt,
        public string $author,
        #[StreamedName('like_count')]
        public int $likeCount,
    ) {
    }

    /**
     * A row of PostRepository's read model query.
     *
     * @param PostRow $row
     */
    public static function fromRow(array $row): self
    {
        return new self(
            $row['id'],
            $row['body'],
            $row['created_at'],
            $row['username'],
            $row['like_count'],
        );
    }

    /**
     * For a post that was just created, so it has no likes yet.
     * The author's username is passed in: reading it from the post's User reference would query the users table.
     */
    public static function fromPost(Post $post, string $author): self
    {
        return new self(
            $post->getId() ?? throw new \LogicException('post id is set by flush()'),
            $post->getBody(),
            $post->getCreatedAt(),
            $author,
            0,
        );
    }
}
