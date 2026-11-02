<?php

declare(strict_types=1);

namespace App\Controller;

use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpKernel\Exception\BadRequestHttpException;

/**
 * Request body of POST /posts: `{"body":"..."}`, its body trimmed.
 */
final readonly class CreatePostPayload
{
    private function __construct(
        public string $body,
    ) {
    }

    /**
     * @throws BadRequestHttpException when the body isn't JSON, or `body` is missing, not a string, blank, or longer
     *                                 than 500 characters once trimmed
     */
    public static function fromRequest(Request $request): self
    {
        try {
            $payload = json_decode($request->getContent(), true, flags: \JSON_THROW_ON_ERROR);
        } catch (\JsonException $jsonException) {
            throw new BadRequestHttpException('malformed JSON body', $jsonException);
        }

        $body = \is_array($payload) ? $payload['body'] ?? null : null;
        if (!\is_string($body) || '' === $body = trim($body)) {
            throw new BadRequestHttpException('body is required');
        }

        if (mb_strlen($body) > 500) {
            throw new BadRequestHttpException('body must be at most 500 characters');
        }

        return new self($body);
    }
}
