import Fastify, { type FastifyReply, type FastifyRequest } from 'fastify';
import jwt from '@fastify/jwt';
import Database from 'better-sqlite3';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const { SQLITE_PATH, JWT_SECRET, HOST, PORT } = process.env;

if (!SQLITE_PATH || !JWT_SECRET || !HOST || !PORT) {
  throw new Error('SQLITE_PATH, JWT_SECRET, HOST and PORT are required');
}

const db = new Database(SQLITE_PATH, {
  fileMustExist: true,
  nativeBinding: fileURLToPath(
    new URL(
      '../node_modules/better-sqlite3/build/Release/better_sqlite3.node',
      import.meta.url
    )
  )
});
db.pragma('journal_mode = WAL');
db.pragma('synchronous = NORMAL');
db.pragma('foreign_keys = ON');
db.pragma('cache_size = -65536');
db.pragma('mmap_size = 268435456');

const jsonPost = `json_object('id', p.id, 'body', p.body, 'created_at', p.created_at,
  'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))`;
const feed = db
  .prepare<[], string>(
    `SELECT '{"posts":[' || coalesce(group_concat(entry), '') || ']}' FROM
  (SELECT ${jsonPost} AS entry FROM posts p JOIN users u ON u.id = p.user_id
   ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`
  )
  .pluck();

type Post = {
  id: number;
  body: string;
  created_at: string;
  author: string;
  like_count: number;
};

const post = db.prepare<
  [number],
  Post
>(`SELECT p.id, p.body, p.created_at, u.username AS author,
  (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`);
const exists = db.prepare<[number]>('SELECT 1 FROM posts WHERE id = ?');
const health = db.prepare('SELECT 1');
const insert = db.prepare<[number, string], { id: number; created_at: string }>(
  'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at'
);
const like = db.prepare<[number, number]>(
  'INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING'
);

type WriteJob = {
  run: () => unknown;
  finish: (failure: unknown, result?: unknown) => void;
  result?: unknown;
  failure?: unknown;
};

let pending: WriteJob[] = [];
let scheduled: NodeJS.Immediate | undefined;

const commit = db.transaction((jobs: WriteJob[]) => {
  for (const job of jobs) {
    try {
      job.result = job.run();
    } catch (failure) {
      // FK/constraint ABORT affects one statement. FULL/IO errors may roll back
      // the whole transaction; never let subsequent jobs run in autocommit.
      if (!db.inTransaction) throw failure;
      job.failure = failure;
    }
  }
});

function flushWrites() {
  if (scheduled) clearImmediate(scheduled);
  scheduled = undefined;
  const jobs = pending;
  pending = [];
  // A transaction-level failure must never acknowledge an uncommitted write.
  let failure: unknown;
  try {
    commit(jobs);
  } catch (err) {
    failure = err;
  }
  for (const job of jobs) job.finish(failure ?? job.failure, job.result);
}

function write<T>(
  run: () => T,
  finish: (failure: unknown, result?: T) => void
) {
  pending.push({
    run,
    finish: (failure, result) => finish(failure, result as T)
  });
  if (pending.length >= 128) flushWrites();
  else if (!scheduled) scheduled = setImmediate(flushWrites);
}

const app = Fastify({ logger: false, keepAliveTimeout: 75000 });
const parseJson = app.getDefaultJsonParser('error', 'error');
app.addContentTypeParser(
  'application/json',
  { parseAs: 'string' },
  (request, body, done) => {
    // The official workload sends an empty like body with a JSON content type.
    if (body === '' && request.routeOptions.url === '/posts/:id/like')
      return done(null, null);
    parseJson(request, body as string, done);
  }
);
// Retire completed responses under descriptor pressure, without interrupting
// uploads or writes. This tracks transport resources, never application data.
let connectionBudget = 60000;
try {
  const softLimit = /Max open files\s+(\d+)/.exec(
    readFileSync('/proc/self/limits', 'utf8')
  );
  if (softLimit)
    connectionBudget = Math.max(
      1,
      Math.min(connectionBudget, Number(softLimit[1]) - 128)
    );
} catch {
  /* /proc is unavailable outside Linux; retain the conservative budget. */
}
let connections = 0;
app.server.on('connection', socket => {
  connections++;
  socket.once('close', () => {
    connections--;
  });
});

app.addHook('onSend', (_request, reply, payload, done) => {
  if (connections >= connectionBudget) reply.header('connection', 'close');
  done(null, payload);
});
await app.register(jwt, {
  secret: JWT_SECRET,
  verify: { algorithms: ['HS256'], cache: false }
});
const postSchema = {
  type: 'object',
  required: ['id', 'body', 'created_at', 'author', 'like_count'],
  properties: {
    id: { type: 'integer' },
    body: { type: 'string' },
    created_at: { type: 'string' },
    author: { type: 'string' },
    like_count: { type: 'integer' }
  }
};
const singleSchema = {
  type: 'object',
  required: ['post'],
  properties: { post: postSchema }
};
const error = (reply: FastifyReply, status: number, message: string) =>
  reply.code(status).send({ error: message });
function idOf(value: string): number | undefined {
  if (!/^[0-9]+$/.test(value)) return;
  const id = Number(value);
  return Number.isSafeInteger(id) && id > 0 ? id : undefined;
}

type Identity = { id: number; username: string };

declare module 'fastify' {
  interface FastifyRequest {
    identity: Identity | null;
  }
}
app.decorateRequest('identity', null);

function authenticate(
  request: FastifyRequest,
  reply: FastifyReply,
  done: (err?: Error) => void
) {
  const auth = request.headers.authorization;
  if (!auth?.startsWith('Bearer ')) {
    error(reply, 401, 'missing bearer token');
    return;
  }
  let payload: { sub?: unknown; username?: unknown };
  try {
    payload = app.jwt.verify(auth.slice(7));
  } catch {
    error(reply, 401, 'invalid or expired token');
    return;
  }
  if (!payload || typeof payload !== 'object') {
    error(reply, 401, 'invalid token payload');
    return;
  }
  const id = typeof payload.sub === 'string' ? idOf(payload.sub) : undefined;
  if (!id || typeof payload.username !== 'string') {
    error(reply, 401, 'invalid token payload');
    return;
  }
  request.identity = { id, username: payload.username };
  done();
}

app.setNotFoundHandler((_request, reply) => error(reply, 404, 'not found'));

app.setErrorHandler((err, _request, reply) => {
  const code = (err as { code?: string }).code;
  if (
    code === 'FST_ERR_CTP_INVALID_JSON_BODY' ||
    code === 'FST_ERR_CTP_EMPTY_JSON_BODY'
  ) {
    return error(reply, 400, 'malformed JSON body');
  }
  console.error(err);
  return error(reply, 500, 'internal server error');
});

app.get('/health', (_request, reply) => {
  try {
    health.get();
    return reply.send({
      status: 'ok',
      db: 'ok',
      uptime_s: Math.floor(process.uptime())
    });
  } catch (err) {
    return reply
      .code(503)
      .send({ status: 'degraded', db: 'unreachable', error: String(err) });
  }
});

app.get(
  '/feed',
  {
    schema: {
      response: {
        200: {
          type: 'object',
          required: ['posts'],
          properties: { posts: { type: 'array', items: postSchema } }
        }
      }
    }
  },
  (_request, reply) => reply.type('application/json').send(feed.get())
);
app.get<{ Params: { id: string } }>(
  '/posts/:id',
  { schema: { response: { 200: singleSchema } } },
  (request, reply) => {
    const id = idOf(request.params.id);
    if (!id) return error(reply, 400, 'invalid post id');
    const result = post.get(id);
    return result
      ? reply.send({ post: result })
      : error(reply, 404, 'post not found');
  }
);
app.post<{ Body: { body?: unknown } | null }>(
  '/posts',
  {
    onRequest: authenticate,
    schema: { response: { 201: singleSchema } }
  },
  (request, reply) => {
    const value = request.body?.body;
    if (typeof value !== 'string' || !value.trim())
      return error(reply, 400, 'body is required');
    const body = value.trim();
    // SQLite length() and the API count Unicode code points, not UTF-16 units.
    if (body.length > 500 && [...body].length > 500)
      return error(reply, 400, 'body must be at most 500 characters');
    const user = request.identity!;
    write(
      () => insert.get(user.id, body)!,
      (failure, result) => {
        if (failure) {
          console.error(failure);
          error(reply, 500, 'internal server error');
          return;
        }
        reply.code(201).send({
          post: {
            id: result!.id,
            body,
            created_at: result!.created_at,
            author: user.username,
            like_count: 0
          }
        });
      }
    );
    return reply;
  }
);
app.post<{ Params: { id: string } }>(
  '/posts/:id/like',
  { onRequest: authenticate },
  (request, reply) => {
    const id = idOf(request.params.id);
    if (!id) return error(reply, 400, 'invalid post id');
    if (!exists.get(id)) return error(reply, 404, 'post not found');
    write(
      () => like.run(request.identity!.id, id).changes !== 0,
      (failure, changed) => {
        if (failure) {
          console.error(failure);
          error(reply, 500, 'internal server error');
          return;
        }
        reply
          .code(changed ? 201 : 200)
          .send({ liked: true, already_liked: !changed, post_id: id });
      }
    );
    return reply;
  }
);
app.addHook('onClose', (_instance, done) => {
  db.close();
  done();
});

for (const signal of ['SIGTERM', 'SIGINT'] as const) {
  process.once(signal, () => {
    void app.close().catch(err => {
      console.error(err);
      process.exitCode = 1;
    });
  });
}

await app.listen({ host: HOST, port: Number(PORT), backlog: 4096 });
