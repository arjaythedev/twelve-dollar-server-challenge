<?php

declare(strict_types=1);

namespace App\Controller;

use App\Db\PostRepository;
use App\Http\Json;
use App\Kernel;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Attribute\AsController;
use Symfony\Component\Routing\Attribute\Route;

#[AsController]
final class HealthController
{
    public function __construct(private readonly PostRepository $posts)
    {
    }

    #[Route('/health', methods: ['GET'])]
    public function __invoke(): Response
    {
        try {
            $this->posts->ping();
        } catch (\Throwable $e) {
            return Json::ok(['status' => 'degraded', 'db' => 'unreachable', 'error' => $e->getMessage()], Response::HTTP_SERVICE_UNAVAILABLE);
        }

        return Json::ok(['status' => 'ok', 'db' => 'ok', 'uptime_s' => time() - Kernel::$startedAt]);
    }
}
