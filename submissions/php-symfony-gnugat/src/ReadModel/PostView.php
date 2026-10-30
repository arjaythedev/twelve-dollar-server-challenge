<?php

declare(strict_types=1);

namespace App\ReadModel;

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
}
