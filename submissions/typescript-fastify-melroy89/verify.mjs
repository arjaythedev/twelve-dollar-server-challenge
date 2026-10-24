// Additional correctness checks. Always uses a temporary copy of the seed.
// Usage: node verify.mjs ../../seed/feed.db
import assert from 'node:assert/strict';
import { copyFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createHmac } from 'node:crypto';
import Database from 'better-sqlite3';

const directory = mkdtempSync(join(tmpdir(), 'fastify-verify-'));
const path = join(directory, 'feed.db');
copyFileSync(process.argv[2] ?? '../../seed/feed.db', path);
const db = new Database(path);
const schema = db.prepare('SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY name').all();
const secret = 'verification-only-secret';
const token = (payload, key = secret) => {
  const input = [ { alg: 'HS256', typ: 'JWT' }, payload ].map(v => Buffer.from(JSON.stringify(v)).toString('base64url')).join('.');
  return `${input}.${createHmac('sha256', key).update(input).digest('base64url')}`;
};
const valid = token({ sub: '1', username: 'verification', exp: Math.floor(Date.now() / 1000) + 3600 });
let child;
const base = 'http://127.0.0.1:3197';
async function start() {
  child = spawn(process.execPath, ['dist/server.js'], {
    env: { ...process.env, SQLITE_PATH: path, JWT_SECRET: secret, HOST: '127.0.0.1', PORT: '3197' }, stdio: ['ignore', 'ignore', 'pipe']
  });
  let logs = '';
  child.stderr.on('data', data => { logs += data; });
  for (let i = 0; i < 100; i++) {
    if (child.exitCode !== null) throw new Error(logs);
    try { if ((await fetch(`${base}/health`)).ok) return; } catch {}
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error(`server did not start: ${logs}`);
}
async function stop(signal = 'SIGTERM') {
  const exited = once(child, 'exit'); child.kill(signal); await exited;
}
async function request(route, method = 'GET', body, auth = valid) {
  const response = await fetch(`${base}${route}`, { method, headers: {
    authorization: `Bearer ${auth}`, ...(body === undefined ? {} : { 'content-type': 'application/json' })
  }, body: body === undefined ? undefined : JSON.stringify(body) });
  return { status: response.status, body: await response.json() };
}
try {
  await start();
  const initial = await request('/posts/1');
  db.prepare('UPDATE posts SET body = ? WHERE id = 1').run('external change');
  assert.equal((await request('/posts/1')).body.post.body, 'external change');
  db.prepare('UPDATE posts SET body = ? WHERE id = 1').run(initial.body.post.body);
  assert.equal((await request('/posts/1/like', 'POST', undefined, token({ sub: '1', username: 'verification', exp: 1 }))).status, 401);
  assert.equal((await request('/posts', 'POST', { body: 'x' }, token({ sub: '1', username: 'verification' }, 'wrong-secret'))).status, 401);
  const emptyCreate = await fetch(`${base}/posts`, { method: 'POST', headers: {
    authorization: `Bearer ${valid}`, 'content-type': 'application/json'
  } });
  assert.equal(emptyCreate.status, 400);
  assert.deepEqual(await emptyCreate.json(), { error: 'malformed JSON body' });
  const unicode = await request('/posts', 'POST', { body: '😀'.repeat(500) });
  assert.equal(unicode.status, 201);
  assert.equal((await request('/posts', 'POST', { body: '😀'.repeat(501) })).status, 400);
  const id = unicode.body.post.id;
  const emptyLike = await fetch(`${base}/posts/${id}/like`, { method: 'POST', headers: {
    authorization: `Bearer ${valid}`, 'content-type': 'application/json'
  } });
  assert.equal(emptyLike.status, 201);
  await emptyLike.arrayBuffer();
  db.prepare('DELETE FROM likes WHERE post_id = ?').run(id);
  const duplicates = await Promise.all(Array.from({ length: 32 }, () => request(`/posts/${id}/like`, 'POST')));
  assert.equal(duplicates.filter(r => r.status === 201).length, 1);
  assert.equal(duplicates.filter(r => r.status === 200).length, 31);
  assert.equal((await request(`/posts/${id}`)).body.post.like_count, 1);
  assert.equal((await request('/posts', 'POST', { body: 'invalid FK' }, token({ sub: '999999999', username: 'unknown' }))).status, 500);
  const mixed = await Promise.all(Array.from({ length: 32 }, (_, i) => request('/posts', 'POST',
    { body: `mixed ${i}` }, i % 4 ? valid : token({ sub: '999999999', username: 'unknown' }))));
  assert.equal(mixed.filter(r => r.status === 201).length, 24);
  assert.equal(mixed.filter(r => r.status === 500).length, 8);
  for (const result of mixed.filter(r => r.status === 201)) {
    assert.equal(db.prepare('SELECT body FROM posts WHERE id = ?').get(result.body.post.id).body, result.body.post.body);
  }
  const created = await Promise.all(Array.from({ length: 32 }, (_, i) => request('/posts', 'POST', { body: `durable ${i}` })));
  assert(created.every(r => r.status === 201));
  for (const result of created) assert.equal(db.prepare('SELECT body FROM posts WHERE id = ?').get(result.body.post.id).body, result.body.post.body);
  await stop('SIGKILL');
  for (const result of created) assert.equal(db.prepare('SELECT body FROM posts WHERE id = ?').get(result.body.post.id).body, result.body.post.body);
  assert.equal(db.prepare('SELECT count(*) AS n FROM likes WHERE post_id = ?').get(id).n, 1);
  await start();
  assert.equal((await request(`/posts/${id}`)).body.post.like_count, 1);
  assert.deepEqual(db.prepare('SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY name').all(), schema);
  console.log('PASS: fresh reads, uncached JWT checks, Unicode, concurrent duplicate likes, FK recovery, acknowledged-write SIGKILL persistence, restart and unchanged schema');
} finally {
  if (child?.exitCode === null && child?.signalCode === null) await stop();
  db.close(); rmSync(directory, { recursive: true, force: true });
}
