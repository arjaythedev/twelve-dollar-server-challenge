export class PostNotFoundError extends Error {
  override readonly name = 'PostNotFoundError'
}

export const postNotFound = new PostNotFoundError('post not found')
