<?php

declare(strict_types=1);

namespace App\Entity;

use App\Controller\CreatePostPayload;
use App\Repository\PostRepository;
use Doctrine\ORM\Mapping as ORM;

#[ORM\Entity(repositoryClass: PostRepository::class)]
#[ORM\Table(name: 'posts')]
class Post
{
    #[ORM\Id]
    #[ORM\GeneratedValue]
    #[ORM\Column]
    private ?int $id = null;

    /**
     * Stored as TEXT in the wire format (2025-12-31T23:59:19.409Z), set by the column default.
     */
    #[ORM\Column(insertable: false, updatable: false, generated: 'INSERT')]
    private string $createdAt;

    private function __construct(
        #[ORM\ManyToOne]
        #[ORM\JoinColumn(name: 'user_id', nullable: false)]
        private User $author,
        #[ORM\Column(length: 500)]
        private string $body,
    ) {
    }

    public static function fromPayload(User $author, CreatePostPayload $payload): self
    {
        if (!\is_string($payload->body)) {
            throw new \LogicException('body must be validated first');
        }

        return new self(
            $author,
            trim($payload->body),
        );
    }

    public function getId(): ?int
    {
        return $this->id;
    }

    public function getAuthor(): User
    {
        return $this->author;
    }

    public function getBody(): string
    {
        return $this->body;
    }

    public function getCreatedAt(): string
    {
        return $this->createdAt;
    }
}
