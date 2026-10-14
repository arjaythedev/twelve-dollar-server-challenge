import { Database } from "bun:sqlite";

export interface FeedPost {
  id: number;
  body: string;
  created_at: string;
  author: string;
  like_count: number;
}

interface PostRow {
  id: number;
  body: string;
  created_at: string;
  username: string;
  like_count: number;
}

const POST_SELECT = `
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
`;
const FEED_SQL = `${POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`;
const POST_BY_ID_SQL = `${POST_SELECT} WHERE p.id = ?`;
const INSERT_POST_SQL = `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`;
const INSERT_LIKE_SQL = `
INSERT INTO likes (user_id, post_id)
SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
ON CONFLICT (user_id, post_id) DO NOTHING
`;
const POST_EXISTS_SQL = `SELECT 1 FROM posts WHERE id = ?`;

let db: Database;
let qHealth: ReturnType<Database["query"]>;
let qFeed: ReturnType<Database["query"]>;
let qPostById: ReturnType<Database["query"]>;
let qInsertPost: ReturnType<Database["query"]>;
let qInsertLike: ReturnType<Database["query"]>;
let qPostExists: ReturnType<Database["query"]>;

export function openDatabase(sqlitePath: string): void {
  db = new Database(sqlitePath, { create: false, readwrite: true });
  db.exec(`PRAGMA journal_mode = WAL`);
  db.exec(`PRAGMA synchronous = NORMAL`);
  db.exec(`PRAGMA busy_timeout = 5000`);
  db.exec(`PRAGMA mmap_size = 536870912`);
  db.exec(`PRAGMA cache_size = -65536`);
  db.exec(`PRAGMA temp_store = MEMORY`);
  db.exec(`PRAGMA journal_size_limit = 67108864`);
  startBackgroundCheckpoints(sqlitePath);

  qHealth = db.query(`SELECT 1`);
  qFeed = db.query(FEED_SQL);
  qPostById = db.query(POST_BY_ID_SQL);
  qInsertPost = db.query(INSERT_POST_SQL);
  qInsertLike = db.query(INSERT_LIKE_SQL);
  qPostExists = db.query(POST_EXISTS_SQL);
}

// Checkpoints (and their fsync) run on a worker thread so they never block the event loop.
// If the worker dies, fall back to SQLite's own auto-checkpoint so the WAL can't grow unbounded.
function startBackgroundCheckpoints(sqlitePath: string): void {
  db.exec(`PRAGMA wal_autocheckpoint = 0`);
  const worker = new Worker(new URL("./checkpoint.ts", import.meta.url).href);
  const fallback = () => db.exec(`PRAGMA wal_autocheckpoint = 1000`);
  worker.addEventListener("error", fallback);
  worker.addEventListener("close", fallback);
  worker.postMessage(sqlitePath);
}

function toFeedPost(row: PostRow): FeedPost {
  return {
    id: row.id,
    body: row.body,
    created_at: row.created_at,
    author: row.username,
    like_count: row.like_count,
  };
}

export function checkHealth(): void {
  (qHealth.get() as unknown) as void;
}

export function listNewestPosts(): FeedPost[] {
  const rows = qFeed.all() as unknown as PostRow[];
  return rows.map(toFeedPost);
}

export function findPostById(id: number): FeedPost | null {
  const row = qPostById.get(id) as unknown as PostRow | null;
  return row ? toFeedPost(row) : null;
}

export function createPost(userId: number, body: string): { id: number; created_at: string } {
  const row = qInsertPost.get(userId, body) as unknown as {
    id: number;
    created_at: string;
  };
  return row;
}

export type LikeOutcome = "created" | "already" | "missing";

export function createLike(userId: number, postId: number): LikeOutcome {
  const result = qInsertLike.run(userId, postId) as unknown as { changes: number };
  if (result.changes === 1) return "created";
  const exists = qPostExists.get(postId) as unknown as Record<string, number> | null;
  return exists ? "already" : "missing";
}
