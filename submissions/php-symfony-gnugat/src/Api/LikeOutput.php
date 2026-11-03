<?php

declare(strict_types=1);

namespace App\Api;

use Symfony\Component\JsonStreamer\Attribute\JsonStreamable;
use Symfony\Component\JsonStreamer\Attribute\StreamedName;

/**
 * POST /posts/{id}/like.
 */
#[JsonStreamable]
final readonly class LikeOutput
{
    public function __construct(
        public bool $liked,
        #[StreamedName('already_liked')]
        public bool $alreadyLiked,
        #[StreamedName('post_id')]
        public int $postId,
    ) {
    }
}
