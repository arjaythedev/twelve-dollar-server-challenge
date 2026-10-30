<?php

declare(strict_types=1);

namespace App\ReadModel;

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
}
