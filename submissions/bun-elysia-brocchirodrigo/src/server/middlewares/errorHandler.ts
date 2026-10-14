import { Elysia, NotFound, ParseError, status, ValidationError } from 'elysia'

const fail = (code: number, error: string) => status(code, { error })

export const errorHandler = new Elysia({ name: 'errorHandler' }).onError('global', ({ error }) => {
  if (error instanceof ValidationError) {
    return fail(400, typeof error.customError === 'string' ? error.customError : 'invalid request')
  }
  if (error instanceof NotFound) return fail(404, 'not found')
  if (error instanceof ParseError) return fail(400, 'malformed JSON body')
  console.error(error)
  return fail(500, 'internal server error')
})
