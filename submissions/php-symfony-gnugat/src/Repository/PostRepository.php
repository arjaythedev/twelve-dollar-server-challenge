<?php

declare(strict_types=1);

namespace App\Repository;

use App\Entity\Post;
use App\ReadModel\PostView;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
use Doctrine\ORM\Query;
use Doctrine\ORM\QueryBuilder;
use Doctrine\Persistence\ManagerRegistry;

/**
 * @extends ServiceEntityRepository<Post>
 */
class PostRepository extends ServiceEntityRepository
{
    public function __construct(ManagerRegistry $registry)
    {
        parent::__construct($registry, Post::class);
    }

    /**
     * @return list<PostView>
     */
    public function findLatestViews(int $limit): array
    {
        return $this->createViewQueryBuilder()
            ->orderBy('p.createdAt', \SortDirection::Descending)
            ->addOrderBy('p.id', \SortDirection::Descending)
            ->setMaxResults($limit)
            ->getQuery()
            ->getResult();
    }

    public function findView(int $id): ?PostView
    {
        return $this->createViewQueryBuilder()
            ->where('p.id = :id')
            ->setParameter('id', $id)
            ->getQuery()
            ->getOneOrNullResult(Query::HYDRATE_OBJECT);
    }

    /**
     * The read model: one row per post, with its author and its like count as of now.
     *
     * Counting likes.post_id (rather than a non-key column) lets SQLite answer from likes_post_id_idx alone.
     */
    private function createViewQueryBuilder(): QueryBuilder
    {
        return $this->createQueryBuilder('p')
            ->select(<<<'DQL'
            NEW App\ReadModel\PostView(
                p.id,
                p.body,
                p.createdAt,
                u.username,
                (SELECT COUNT(IDENTITY(l.post)) FROM App\Entity\PostLike l WHERE l.post = p)
            )
            DQL)
            ->join('p.author', 'u');
    }
}
