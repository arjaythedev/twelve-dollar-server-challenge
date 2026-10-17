import {
  bodyRequired,
  bodyTooLong,
  type InvalidPostBodyError,
} from '@modules/posts/domain/exceptions/invalidPostBodyError'

const postBodyMaxLength = 500

const countCharacters = (value: string): number => {
  let count = 0
  for (const _ of value) count++
  return count
}

export const parsePostBody = (raw: string): string | InvalidPostBodyError => {
  const value = raw.trim()
  if (value.length === 0) return bodyRequired
  if (value.length > postBodyMaxLength && countCharacters(value) > postBodyMaxLength) return bodyTooLong
  return value
}
