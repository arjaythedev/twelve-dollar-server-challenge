<?php

declare(strict_types=1);

namespace App\Entity;

use App\Controller\CreatePostPayload;
use App\Repository\PostRepository;
use Doctrine\Common\Collections\ArrayCollection;
use Doctrine\Common\Collections\Collection;
use Doctrine\ORM\Mapping as ORM;
use Symfony\Component\Serializer\Attribute\Ignore;
use Symfony\Component\Serializer\Attribute\SerializedName;

/**
 * Also the post object of the API, serialized in property order (then getter-only fields): keep it.
 */
#[ORM\Entity(repositoryClass: PostRepository::class)]
#[ORM\Table(name: 'posts')]
class Post
{
    #[ORM\Id]
    #[ORM\GeneratedValue]
    #[ORM\Column]
    private ?int $id = null;

    #[ORM\Column(length: 500)]
    private string $body;

    /**
     * Stored as TEXT in the wire format (2025-12-31T23:59:19.409Z), set by the column default.
     */
    #[ORM\Column(insertable: false, updatable: false, generated: 'INSERT')]
    private string $createdAt;

    #[ORM\ManyToOne]
    #[ORM\JoinColumn(name: 'user_id', nullable: false)]
    private User $author;

    /**
     * Extra lazy: counting them is a COUNT query, they're never loaded.
     *
     * @var Collection<int, PostLike>
     */
    #[ORM\OneToMany(targetEntity: PostLike::class, mappedBy: 'post', fetch: 'EXTRA_LAZY')]
    private Collection $likes;

    private function __construct(User $author, string $body)
    {
        $this->author = $author;
        $this->body = $body;
        $this->likes = new ArrayCollection();
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

    #[Ignore]
    public function getAuthor(): User
    {
        return $this->author;
    }

    public function getBody(): string
    {
        return $this->body;
    }

    #[SerializedName('created_at')]
    public function getCreatedAt(): string
    {
        return $this->createdAt;
    }

    #[SerializedName('author')]
    public function getAuthorUsername(): string
    {
        return $this->author->getUsername();
    }

    #[SerializedName('like_count')]
    public function getLikeCount(): int
    {
        return $this->likes->count();
    }
}
