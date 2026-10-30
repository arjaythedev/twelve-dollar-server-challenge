<?php

declare(strict_types=1);

namespace App\Entity;

use App\Repository\PostLikeRepository;
use Doctrine\ORM\Mapping as ORM;

#[ORM\Entity(repositoryClass: PostLikeRepository::class)]
#[ORM\Table(name: 'likes')]
class PostLike
{
    #[ORM\Column(insertable: false, updatable: false, generated: 'INSERT')]
    private string $createdAt;

    public function __construct(
        #[ORM\Id]
        #[ORM\ManyToOne]
        #[ORM\JoinColumn(name: 'user_id')]
        private User $user,
        #[ORM\Id]
        #[ORM\ManyToOne]
        #[ORM\JoinColumn(name: 'post_id')]
        private Post $post,
    ) {
    }

    public function getUser(): User
    {
        return $this->user;
    }

    public function getPost(): Post
    {
        return $this->post;
    }

    public function getCreatedAt(): string
    {
        return $this->createdAt;
    }
}
