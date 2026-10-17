export class AuthenticationError extends Error {
  override readonly name = 'AuthenticationError'
}

export const missingBearerToken = new AuthenticationError('missing bearer token')
export const invalidToken = new AuthenticationError('invalid or expired token')
export const invalidTokenPayload = new AuthenticationError('invalid token payload')
