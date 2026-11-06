<?php

declare(strict_types=1);

namespace App\Outlets;

use Pype\Http\Response;
use Pype\Outlet\JsonOutlet;

/** The spec's envelope: the handler's array as-is, errors as {"error":"<message>"} (a failed validation: its first message). */
final class ApiOutlet extends JsonOutlet
{
    protected function payload(Response $response): array
    {
        if ($response->isError()) {
            $error = $response->errorDetails();

            return ['error' => isset($error['errors']) ? array_values($error['errors'])[0][0] : $error['message']];
        }

        return $response->data();
    }
}
