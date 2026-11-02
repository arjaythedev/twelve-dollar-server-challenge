<?php

declare(strict_types=1);

namespace App\EventListener;

use Psr\Log\LoggerInterface;
use Symfony\Component\EventDispatcher\Attribute\AsEventListener;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Event\ExceptionEvent;
use Symfony\Component\HttpKernel\Exception\HttpExceptionInterface;
use Symfony\Component\HttpKernel\KernelEvents;
use Symfony\Component\Routing\Exception\ResourceNotFoundException;

/**
 * Every error is `{"error":"<message>"}`. Runs after the Security listener, which handles 401s.
 */
#[AsEventListener(KernelEvents::EXCEPTION, priority: -64)]
final readonly class JsonErrorListener
{
    public function __construct(
        private LoggerInterface $logger,
    ) {
    }

    public function __invoke(ExceptionEvent $event): void
    {
        $exception = $event->getThrowable();
        if (!$exception instanceof HttpExceptionInterface) {
            $this->logger->error('Uncaught exception', ['exception' => $exception]);
            $event->setResponse(new JsonResponse(['error' => 'internal server error'], Response::HTTP_INTERNAL_SERVER_ERROR));

            return;
        }

        $message = $exception->getPrevious() instanceof ResourceNotFoundException
            ? 'not found'
            : $exception->getMessage();

        $event->setResponse(new JsonResponse(['error' => $message], $exception->getStatusCode(), $exception->getHeaders()));
    }
}
