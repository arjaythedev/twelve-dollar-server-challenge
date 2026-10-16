// bench/load.js's request mix with the sleeps removed: closed-loop saturation, measures max req/s.
import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS)));

export const options = {
  vus: Number(__ENV.VUS || 64),
  duration: __ENV.DURATION || '30s',
  summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'],
};

export default function () {
  const user = tokens[(__VU - 1) % tokens.length];
  const auth = { headers: { Authorization: `Bearer ${user.token}`, 'Content-Type': 'application/json' } };
  const feed = http.get(`${BASE_URL}/feed`);
  if (!check(feed, { 'feed 200': (r) => r.status === 200 })) return;
  const posts = feed.json('posts');
  const post = posts[Math.floor(Math.random() * posts.length)];
  check(http.get(`${BASE_URL}/posts/${post.id}`), { 'post 200': (r) => r.status === 200 });
  if (Math.random() < 0.15) {
    const r = http.post(`${BASE_URL}/posts/${post.id}/like`, null, auth);
    check(r, { 'like 200/201': (r) => r.status === 200 || r.status === 201 });
  }
  if (Math.random() < 0.02) {
    const body = JSON.stringify({ body: `${user.username} says hi (VU ${__VU}, iter ${__ITER})` });
    check(http.post(`${BASE_URL}/posts`, body, auth), { 'create 201': (r) => r.status === 201 });
  }
}
