<?php

declare(strict_types=1);

namespace App\Api;

use App\ReadModel\PostView;
use Symfony\Component\JsonStreamer\Attribute\JsonStreamable;

/**
 * GET /posts/{id} and POST /posts.
 */
#[JsonStreamable]
final readonly class PostOutput
{
    public function __construct(
        public PostView $post,
    ) {
    }
}
