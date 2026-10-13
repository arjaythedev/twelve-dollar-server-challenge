<?php

declare(strict_types=1);

namespace App\Db;

use Doctrine\DBAL\ParameterType;
use Doctrine\DBAL\Result;

final class PostRepository
{
    private const string POST = 'SELECT p.id, p.body, p.created_at, u.username AS author,'
        .' (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count'
        .' FROM posts p JOIN users u ON u.id = p.user_id';

    public function __construct(private readonly Db $db)
    {
    }

    /** @return list<array{id: int, body: string, created_at: string, author: string, like_count: int}> */
    public function feed(): array
    {
        $result = $this->db->statement(self::POST.' ORDER BY p.created_at DESC, p.id DESC LIMIT 20')->executeQuery();

        return self::all($result);
    }

    /** @return array{id: int, body: string, created_at: string, author: string, like_count: int}|null */
    public function find(int $id): ?array
    {
        $statement = $this->db->statement(self::POST.' WHERE p.id = ?');
        $statement->bindValue(1, $id, ParameterType::INTEGER);

        return self::one($statement->executeQuery());
    }

    public function exists(int $id): bool
    {
        $statement = $this->db->statement('SELECT 1 FROM posts WHERE id = ?');
        $statement->bindValue(1, $id, ParameterType::INTEGER);

        return null !== self::one($statement->executeQuery());
    }

    /** @return array{id: int, created_at: string} */
    public function create(int $userId, string $body): array
    {
        $statement = $this->db->statement('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at');
        $statement->bindValue(1, $userId, ParameterType::INTEGER);
        $statement->bindValue(2, $body);

        // one() frees the result, which resets the statement: that is the moment SQLite commits.
        return self::one($statement->executeQuery()) ?? throw new \RuntimeException('INSERT ... RETURNING returned no row');
    }

    /** Inserts the like if the post exists and it is new. Returns the number of rows inserted (0 or 1). */
    public function like(int $userId, int $postId): int
    {
        $statement = $this->db->statement(
            'INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)'
            .' ON CONFLICT (user_id, post_id) DO NOTHING'
        );
        $statement->bindValue(1, $userId, ParameterType::INTEGER);
        $statement->bindValue(2, $postId, ParameterType::INTEGER);
        $statement->bindValue(3, $postId, ParameterType::INTEGER);

        return (int) $statement->executeStatement();
    }

    public function ping(): void
    {
        $this->db->statement('SELECT 1')->executeQuery()->free();
    }

    private static function one(Result $result): ?array
    {
        try {
            return $result->fetchAssociative() ?: null;
        } finally {
            $result->free();
        }
    }

    private static function all(Result $result): array
    {
        try {
            return $result->fetchAllAssociative();
        } finally {
            $result->free();
        }
    }
}
