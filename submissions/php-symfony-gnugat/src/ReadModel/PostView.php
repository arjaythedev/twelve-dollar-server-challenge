<?php

declare(strict_types=1);

namespace App\ReadModel;

use App\Entity\Post;
use Symfony\Component\Serializer\Attribute\SerializedName;

/**
 * The post object of the API: properties are serialized in this order.
 */
final readonly class PostView
{
    public function __construct(
        public int $id,
        public string $body,
        #[SerializedName('created_at')]
        public string $createdAt,
        public string $author,
        #[SerializedName('like_count')]
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
