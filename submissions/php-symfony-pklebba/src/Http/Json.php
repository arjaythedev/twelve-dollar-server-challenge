<?php

declare(strict_types=1);

namespace App\Http;

use Symfony\Component\HttpFoundation\Response;

final class Json
{
    private const int FLAGS = \JSON_UNESCAPED_SLASHES | \JSON_UNESCAPED_UNICODE | \JSON_THROW_ON_ERROR;

    public static function ok(array $data, int $status = Response::HTTP_OK): Response
    {
        return new Response(json_encode($data, self::FLAGS), $status, ['Content-Type' => 'application/json']);
    }

    public static function error(int $status, string $message): Response
    {
        return new Response(json_encode(['error' => $message], self::FLAGS), $status, ['Content-Type' => 'application/json']);
    }
}
