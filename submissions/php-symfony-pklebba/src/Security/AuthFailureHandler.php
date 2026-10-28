<?php

declare(strict_types=1);

namespace App\Security;

use App\Http\Json;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\Security\Core\Exception\AuthenticationException;
use Symfony\Component\Security\Http\Authentication\AuthenticationFailureHandlerInterface;

/** A bearer token was presented but rejected: the message comes from JwtTokenHandler. */
final class AuthFailureHandler implements AuthenticationFailureHandlerInterface
{
    public function onAuthenticationFailure(Request $request, AuthenticationException $exception): Response
    {
        $message = $exception instanceof AuthError ? $exception->getMessageKey() : 'invalid or expired token';

        return Json::error(Response::HTTP_UNAUTHORIZED, $message);
    }
}
