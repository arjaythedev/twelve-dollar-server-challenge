import { invalidToken } from '@modules/auth/domain/exceptions/authenticationError'
import type { TokenClaims, VerifyToken } from '@modules/auth/domain/services/tokenVerifier'

const canonicalHeader = Buffer.from('{"alg":"HS256","typ":"JWT"}').toString('base64url')

const decodeJson = (segment: string): unknown => {
  try {
    return JSON.parse(Buffer.from(segment, 'base64url').toString('utf8'))
  } catch {
    return undefined
  }
}

const isObject = (value: unknown): value is TokenClaims => typeof value === 'object' && value !== null

const isHs256Header = (segment: string): boolean => {
  if (segment === canonicalHeader) return true
  const header = decodeJson(segment)
  return isObject(header) && header.alg === 'HS256'
}

export const createHs256TokenVerifier = (secret: string): VerifyToken => {
  const keyedHasher = new Bun.CryptoHasher('sha256', secret)

  const signatureMatches = (signingInput: string, signature: string): boolean => {
    const expected = Buffer.from(keyedHasher.copy().update(signingInput).digest('base64url'))
    const actual = Buffer.from(signature)
    return actual.length === expected.length && crypto.timingSafeEqual(actual, expected)
  }

  return (token) => {
    const headerEnd = token.indexOf('.')
    const payloadEnd = token.indexOf('.', headerEnd + 1)
    if (headerEnd < 1 || payloadEnd < 0 || token.includes('.', payloadEnd + 1)) return invalidToken
    if (!isHs256Header(token.slice(0, headerEnd))) return invalidToken
    if (!signatureMatches(token.slice(0, payloadEnd), token.slice(payloadEnd + 1))) return invalidToken

    const claims = decodeJson(token.slice(headerEnd + 1, payloadEnd))
    if (!isObject(claims)) return invalidToken

    const now = Date.now() / 1000
    const { exp, nbf } = claims
    if (exp !== undefined && !(typeof exp === 'number' && now < exp)) return invalidToken
    if (nbf !== undefined && !(typeof nbf === 'number' && now >= nbf)) return invalidToken
    return claims
  }
}
