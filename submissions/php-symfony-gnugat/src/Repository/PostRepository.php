<?php

declare(strict_types=1);

namespace App\Repository;

use App\Entity\Post;
use Doctrine\Bundle\DoctrineBundle\Repository\ServiceEntityRepository;
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
     * With their authors (fetch join), so serializing them doesn't load each author on its own.
     *
     * @return list<Post>
     */
    public function findLatest(int $limit): array
    {
        return $this->createQueryBuilder('p')
            ->addSelect('u')
            ->join('p.author', 'u')
            ->orderBy('p.createdAt', \SortDirection::Descending)
            ->addOrderBy('p.id', \SortDirection::Descending)
            ->setMaxResults($limit)
            ->getQuery()
            ->getResult();
    }
}
