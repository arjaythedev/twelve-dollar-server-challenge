export class InvalidPostBodyError extends Error {
  override readonly name = 'InvalidPostBodyError'
}

export const bodyRequired = new InvalidPostBodyError('body is required')
export const bodyTooLong = new InvalidPostBodyError('body must be at most 500 characters')
