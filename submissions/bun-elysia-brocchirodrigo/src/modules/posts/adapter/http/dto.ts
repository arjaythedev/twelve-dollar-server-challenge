import { t } from 'elysia'

const postIdPattern = String.raw`^[1-9]\d*$`
const postIdRegex = new RegExp(postIdPattern)

export const invalidPostId = 'invalid post id'

export const isValidPostId = (id: string): boolean => postIdRegex.test(id)

export const postIdParams = t.Object({
  id: t.String({ pattern: postIdPattern, error: invalidPostId }),
})

export const createPostBody = t.Object({ body: t.String({ error: 'body is required' }) }, { error: 'body is required' })
