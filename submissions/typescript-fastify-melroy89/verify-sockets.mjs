// Low-descriptor pressure and fragmented upload regression test (Linux).
// Usage: node verify-sockets.mjs ../../seed/feed.db
import assert from 'node:assert/strict';
import { copyFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { connect } from 'node:net';
import { createHmac } from 'node:crypto';

const directory = mkdtempSync(join(tmpdir(), 'fastify-sockets-'));
const path = join(directory, 'feed.db');
copyFileSync(process.argv[2] ?? '../../seed/feed.db', path);
const secret = 'socket-verification-secret';
const input = [{ alg: 'HS256', typ: 'JWT' }, { sub: '1', username: 'socket-test', exp: Math.floor(Date.now() / 1000) + 3600 }]
  .map(value => Buffer.from(JSON.stringify(value)).toString('base64url')).join('.');
const token = `${input}.${createHmac('sha256', secret).update(input).digest('base64url')}`;
const child = spawn('bash', ['-c', 'ulimit -n 256; exec node dist/server.js'], {
  env: { ...process.env, SQLITE_PATH: path, JWT_SECRET: secret, HOST: '127.0.0.1', PORT: '3198' }, stdio: ['ignore', 'ignore', 'pipe']
});
let logs = '';
child.stderr.on('data', chunk => { logs += chunk; });
const sockets = [];
async function socket() {
  const s = connect(3198, '127.0.0.1');
  s.on('error', () => {});
  await once(s, 'connect');
  sockets.push(s);
  return s;
}
function response(s) {
  return new Promise((resolve, reject) => {
    let data = Buffer.alloc(0);
    const timeout = setTimeout(() => finish(new Error('response timeout')), 10000);
    function finish(err, result) {
      clearTimeout(timeout); s.off('data', read); s.off('error', fail); s.off('end', ended);
      err ? reject(err) : resolve(result);
    }
    const fail = err => finish(err);
    const ended = () => finish(new Error('response truncated'));
    function read(chunk) {
      data = Buffer.concat([data, chunk]);
      const split = data.indexOf('\r\n\r\n');
      if (split < 0) return;
      const headers = data.subarray(0, split).toString();
      const length = Number(/content-length: (\d+)/i.exec(headers)?.[1]);
      if (!Number.isFinite(length) || data.length < split + 4 + length) return;
      finish(null, { headers, body: JSON.parse(data.subarray(split + 4, split + 4 + length).toString()) });
    }
    s.on('data', read); s.once('error', fail); s.once('end', ended);
  });
}
try {
  for (let i = 0; i < 100; i++) {
    if (child.exitCode !== null) throw new Error(logs);
    try { if ((await fetch('http://127.0.0.1:3198/health')).ok) break; } catch {}
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  const slow = await socket();
  const body = Buffer.from(JSON.stringify({ body: 'fragmented 😀 upload' }));
  const split = body.indexOf(Buffer.from('😀')) + 2;
  slow.write(`POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer ${token}\r\nContent-Type: application/json\r\nContent-Length: ${body.length}\r\n\r\n`);
  slow.write(body.subarray(0, split));
  // Open idle peers, leaving the upload incomplete while pressure rises.
  for (let i = 0; i < 140; i++) await socket();
  await new Promise(resolve => setTimeout(resolve, 250));
  const complete = response(slow);
  slow.write(body.subarray(split));
  const uploaded = await complete;
  assert.match(uploaded.headers, /^HTTP\/1.1 201/);
  assert.match(uploaded.headers, /connection: close/i);
  assert.equal(uploaded.body.post.body, 'fragmented 😀 upload');
  const reader = await socket();
  const readback = response(reader);
  reader.write(`GET /posts/${uploaded.body.post.id} HTTP/1.1\r\nHost: localhost\r\n\r\n`);
  assert.equal((await readback).body.post.body, 'fragmented 😀 upload');
  for (const s of sockets) s.destroy();
  await new Promise(resolve => setTimeout(resolve, 100));
  const reused = await socket();
  let result = response(reused);
  reused.write('GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n');
  assert.match((await result).headers, /connection: keep-alive/i);
  console.log('PASS: nofile=256, fragmented upload completes under pressure, full committed response and readback; checking 66-second socket reuse');
  await new Promise(resolve => setTimeout(resolve, 66000));
  assert.equal(reused.destroyed, false);
  result = response(reused);
  reused.write('GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n');
  assert.equal((await result).body.status, 'ok');
  console.log('PASS: original socket reused after 66 seconds');
} finally {
  for (const s of sockets) s.destroy();
  if (child.exitCode === null && child.signalCode === null) {
    const exited = once(child, 'exit'); child.kill(); await exited;
  }
  rmSync(directory, { recursive: true, force: true });
}
