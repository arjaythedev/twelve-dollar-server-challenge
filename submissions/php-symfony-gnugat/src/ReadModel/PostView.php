<?php

declare(strict_types=1);

namespace App\ReadModel;

use App\Entity\Post;
use Symfony\Component\JsonStreamer\Attribute\JsonStreamable;
use Symfony\Component\JsonStreamer\Attribute\StreamedName;

/**
 * The post object of the API, for POST /posts: properties are written in this order (the same as PostRepository's
 * json_object(), for the reads).
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
