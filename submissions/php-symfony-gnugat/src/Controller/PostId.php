<?php

declare(strict_types=1);

namespace App\Controller;

use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpKernel\Controller\ValueResolverInterface;
use Symfony\Component\HttpKernel\ControllerMetadata\ArgumentMetadata;
use Symfony\Component\HttpKernel\Exception\BadRequestHttpException;

/**
 * Resolves the {id} route parameter of `#[ValueResolver(PostId::class)] int $id` arguments.
 *
 * A route requirement would turn `/posts/abc` into a 404, but the API wants a 400.
 */
final readonly class PostId implements ValueResolverInterface
{
    /**
     * @return iterable<int>
     */
    public function resolve(Request $request, ArgumentMetadata $argument): iterable
    {
        $id = $request->attributes->getString($argument->getName());
        if (!preg_match('/^[1-9][0-9]*$/D', $id)) {
            throw new BadRequestHttpException('invalid post id');
        }

        return [(int) $id];
    }
}
