<?php

declare(strict_types=1);

namespace App\EventListener;

use App\Http\Json;
use Symfony\Component\EventDispatcher\Attribute\AsEventListener;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Event\ExceptionEvent;
use Symfony\Component\HttpKernel\Exception\HttpExceptionInterface;

#[AsEventListener]
final class ExceptionListener
{
    public function __invoke(ExceptionEvent $event): void
    {
        $throwable = $event->getThrowable();
        if ($throwable instanceof HttpExceptionInterface) {
            $status = $throwable->getStatusCode();
            $message = match ($status) {
                Response::HTTP_NOT_FOUND => 'not found',
                Response::HTTP_METHOD_NOT_ALLOWED => 'method not allowed',
                default => 'internal server error',
            };
            $event->setResponse(Json::error('internal server error' === $message ? Response::HTTP_INTERNAL_SERVER_ERROR : $status, $message));

            return;
        }

        $event->setResponse(Json::error(Response::HTTP_INTERNAL_SERVER_ERROR, 'internal server error'));
    }
}
