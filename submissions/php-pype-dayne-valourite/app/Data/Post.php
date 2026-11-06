<?php

declare(strict_types=1);

namespace App\Data;

/** The post object of the spec. Property order is the JSON key order. */
final readonly class Post
{
    public function __construct(
        public int $id,
        public string $body,
        public string $created_at,
        public string $author,
        public int $like_count,
    ) {
    }
}
