import type { Authenticate } from '@modules/auth/application/useCases/authenticate'
import { AuthenticationError } from '@modules/auth/domain/exceptions/authenticationError'
import { Elysia, status } from 'elysia'

export const authMiddleware = (authenticate: Authenticate) =>
  new Elysia({ name: 'auth' }).derive('plugin', ({ request }) => {
    const user = authenticate(request.headers.get('authorization'))
    return user instanceof AuthenticationError ? status(401, { error: user.message }) : { user }
  })

export type AuthMiddleware = ReturnType<typeof authMiddleware>
