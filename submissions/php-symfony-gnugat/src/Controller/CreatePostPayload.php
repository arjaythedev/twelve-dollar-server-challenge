<?php

declare(strict_types=1);

namespace App\Controller;

use Symfony\Component\Validator\Constraints as Assert;

/**
 * Request body of POST /posts. `mixed` so that `{"body":123}` fails validation (400), not denormalization.
 */
final readonly class CreatePostPayload
{
    public function __construct(
        #[Assert\Sequentially([
            new Assert\Type('string', message: 'body is required'),
            new Assert\NotBlank(message: 'body is required', normalizer: 'trim'),
            new Assert\Length(max: 500, normalizer: 'trim', maxMessage: 'body must be at most 500 characters'),
        ])]
        public mixed $body = null,
    ) {
    }
}
