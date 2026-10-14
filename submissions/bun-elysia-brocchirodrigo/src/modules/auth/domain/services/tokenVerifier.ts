import type { AuthenticationError } from '@modules/auth/domain/exceptions/authenticationError'

export type TokenClaims = Record<string, unknown>

export type VerifyToken = (token: string) => TokenClaims | AuthenticationError
