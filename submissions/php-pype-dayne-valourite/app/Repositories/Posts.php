<?php

declare(strict_types=1);

namespace App\Repositories;

use App\Data\Post;
use Pype\Database\Connection;
use Pype\Database\QueryBuilder;

final class Posts
{
    public function __construct(private readonly Connection $db)
    {
    }

    /** @return list<Post> */
    public function newest(int $limit): array
    {
        return $this->query()->orderBy('p.created_at', 'desc')->orderBy('p.id', 'desc')->limit($limit)->get();
    }

    public function find(int $id): ?Post
    {
        return $this->query()->where('p.id', $id)->first();
    }

    public function exists(int $id): bool
    {
        return $this->db->table('posts')->where('id', $id)->exists();
    }

    /** @return array{0: int, 1: string} the new post's id and created_at (set by the database) */
    public function create(int $userId, string $body): array
    {
        $row = $this->db->table('posts')->insertReturning(['user_id' => $userId, 'body' => $body], ['id', 'created_at']);

        return [(int) $row['id'], $row['created_at']];
    }

    /** @return bool false if the user had already liked it */
    public function like(int $userId, int $postId): bool
    {
        return $this->db->execute('INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING', [$userId, $postId]) === 1;
    }

    /** Post objects: author from users, like_count counted now. */
    private function query(): QueryBuilder
    {
        return $this->db->table('posts as p')
            ->join('users as u', 'u.id', '=', 'p.user_id')
            ->select('p.id', 'p.body', 'p.created_at', 'u.username as author')
            ->selectRaw('(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count')
            ->as(Post::class);
    }
}
