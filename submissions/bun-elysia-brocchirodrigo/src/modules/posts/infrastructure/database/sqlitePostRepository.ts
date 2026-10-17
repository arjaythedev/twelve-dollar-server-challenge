import type { Database } from 'bun:sqlite'
import type { Post } from '@modules/posts/domain/entities/post'
import type { NewPostRecord, PostRepository } from '@modules/posts/domain/repositories/postRepository'

const selectPost = `
  SELECT p.id, p.body, p.created_at AS createdAt, u.username AS author,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS likeCount
    FROM posts p JOIN users u ON u.id = p.user_id`

export const createSqlitePostRepository = (db: Database): PostRepository => {
  const latest = db.query<Post, [number]>(`${selectPost} ORDER BY p.created_at DESC, p.id DESC LIMIT ?`)
  const byId = db.query<Post, [number]>(`${selectPost} WHERE p.id = ?`)
  const insert = db.query<NewPostRecord, [number, string]>(
    'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at AS createdAt',
  )
  const like = db.query<unknown, [number, number]>(
    'INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING',
  )
  const exists = db.query<{ found: number }, [number]>('SELECT 1 AS found FROM posts WHERE id = ?')

  return {
    latest: (limit) => latest.all(limit),
    findById: (id) => byId.get(id),
    create: (authorId, body) => {
      const record = insert.get(authorId, body)
      if (!record) throw new Error('post insert returned no row')
      return record
    },
    addLike: (userId, postId) => like.run(userId, postId).changes === 1,
    exists: (id) => exists.get(id) !== null,
  }
}
