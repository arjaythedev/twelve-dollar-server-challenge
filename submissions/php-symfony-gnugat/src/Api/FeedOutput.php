<?php

declare(strict_types=1);

namespace App\Api;

use App\ReadModel\PostView;
use Symfony\Component\JsonStreamer\Attribute\JsonStreamable;

/**
 * GET /feed.
 */
#[JsonStreamable]
final readonly class FeedOutput
{
    public function __construct(
        /** @var list<PostView> */
        public array $posts,
    ) {
    }
}
