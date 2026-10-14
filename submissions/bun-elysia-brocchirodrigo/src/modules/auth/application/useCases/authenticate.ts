import type { AuthenticatedUser } from '@modules/auth/domain/entities/authenticatedUser'
import {
  AuthenticationError,
  invalidTokenPayload,
  missingBearerToken,
} from '@modules/auth/domain/exceptions/authenticationError'
import type { VerifyToken } from '@modules/auth/domain/services/tokenVerifier'

const bearerPrefix = 'Bearer '
const userIdPattern = /^[1-9]\d{0,15}$/

export type Authenticate = (authorization: string | null) => AuthenticatedUser | AuthenticationError

export const makeAuthenticate =
  (verifyToken: VerifyToken): Authenticate =>
  (authorization) => {
    if (!authorization?.startsWith(bearerPrefix)) return missingBearerToken
    const claims = verifyToken(authorization.slice(bearerPrefix.length))
    if (claims instanceof AuthenticationError) return claims
    const { sub, username } = claims
    if (typeof sub !== 'string' || !userIdPattern.test(sub) || typeof username !== 'string') {
      return invalidTokenPayload
    }
    return { id: Number(sub), username }
  }
