import { desc, sql } from "drizzle-orm"
import { index, integer, primaryKey, sqliteTable, text } from "drizzle-orm/sqlite-core"

export const currentTimestamp = sql<string>`(strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))`

export const users = sqliteTable("users", {
  id: integer("id").primaryKey(),
  username: text("username").notNull().unique(),
  createdAt: text("created_at").notNull().default(currentTimestamp)
})

export const posts = sqliteTable("posts", {
  id: integer("id").primaryKey(),
  userId: integer("user_id").notNull().references(() => users.id),
  body: text("body").notNull(),
  createdAt: text("created_at").notNull().default(currentTimestamp)
}, (table) => [
  index("posts_created_at_id_idx").on(desc(table.createdAt), desc(table.id)),
  index("posts_user_id_idx").on(table.userId)
])

export const likes = sqliteTable("likes", {
  userId: integer("user_id").notNull().references(() => users.id),
  postId: integer("post_id").notNull().references(() => posts.id),
  createdAt: text("created_at").notNull().default(currentTimestamp)
}, (table) => [
  primaryKey({ columns: [table.userId, table.postId] }),
  index("likes_post_id_idx").on(table.postId)
])
