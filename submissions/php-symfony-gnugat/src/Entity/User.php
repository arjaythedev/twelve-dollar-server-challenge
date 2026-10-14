<?php

declare(strict_types=1);

namespace App\Entity;

use App\Repository\UserRepository;
use Doctrine\ORM\Mapping as ORM;

#[ORM\Entity(repositoryClass: UserRepository::class)]
#[ORM\Table(name: 'users')]
class User
{
    #[ORM\Id]
    #[ORM\GeneratedValue]
    #[ORM\Column]
    private ?int $id = null;

    #[ORM\Column(insertable: false, updatable: false, generated: 'INSERT')]
    private string $createdAt;

    public function __construct(
        #[ORM\Column(unique: true)]
        private string $username,
    ) {
    }

    public function getId(): ?int
    {
        return $this->id;
    }

    public function getUsername(): string
    {
        return $this->username;
    }

    public function getCreatedAt(): string
    {
        return $this->createdAt;
    }
}
