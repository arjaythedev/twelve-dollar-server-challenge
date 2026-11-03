<?php

declare(strict_types=1);

namespace App\Security;

use App\Http\Json;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\Security\Core\Exception\AuthenticationException;
use Symfony\Component\Security\Http\EntryPoint\AuthenticationEntryPointInterface;

/** Reached when a protected route gets no "Authorization: Bearer ..." header at all. */
final class AuthEntryPoint implements AuthenticationEntryPointInterface
{
    public function start(Request $request, ?AuthenticationException $authException = null): Response
    {
        return Json::error(Response::HTTP_UNAUTHORIZED, 'missing bearer token');
    }
}
