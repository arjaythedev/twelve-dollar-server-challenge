<?php

declare(strict_types=1);

namespace App\Controller;

use App\Uptime;
use Doctrine\DBAL\Connection;
use Symfony\Bundle\FrameworkBundle\Controller\AbstractController;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\Routing\Attribute\Route;

final class HealthController extends AbstractController
{
    public function __construct(
        private readonly Connection $connection,
        private readonly Uptime $uptime,
    ) {
    }

    #[Route('/health', methods: ['GET'])]
    public function __invoke(): JsonResponse
    {
        try {
            $this->connection->executeQuery('SELECT 1');
        } catch (\Throwable $throwable) {
            return $this->json([
                'status' => 'degraded',
                'db' => 'unreachable',
                'error' => $throwable->getMessage(),
            ], Response::HTTP_SERVICE_UNAVAILABLE);
        }

        return $this->json([
            'status' => 'ok',
            'db' => 'ok',
            'uptime_s' => $this->uptime->seconds(),
        ]);
    }
}
