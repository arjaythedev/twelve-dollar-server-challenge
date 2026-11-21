// Local stress test: same request mix as bench/load.js but with no think time,
// so the server is driven to its CPU limit. Not used for scoring.
//
//   k6 run -e VUS=500 -e DURATION=20s bench_stress.js
import http from 'k6/http';
import { SharedArray } from 'k6/data';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS || '../../seed/tokens.json')));
const VUS = Number(__ENV.VUS || 500);
const DURATION = __ENV.DURATION || '20s';

export const options = {
  scenarios: {
    blast: { executor: 'constant-vus', vus: VUS, duration: DURATION },
  },
  summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'],
};

export default function () {
  const user = tokens[(__VU - 1) % tokens.length];
  const auth = { headers: { Authorization: `Bearer ${user.token}`, 'Content-Type': 'application/json' } };

  const feed = http.get(`${BASE_URL}/feed`, { tags: { name: 'GET /feed' } });
  if (feed.status !== 200) return;
  const posts = feed.json('posts');
  const post = posts[Math.floor(Math.random() * posts.length)];

  http.get(`${BASE_URL}/posts/${post.id}`, { tags: { name: 'GET /posts/:id' } });

  if (Math.random() < 0.15)
    http.post(`${BASE_URL}/posts/${post.id}/like`, null, { ...auth, tags: { name: 'POST like' } });

  if (Math.random() < 0.02)
    http.post(`${BASE_URL}/posts`, JSON.stringify({ body: `${user.username} says hi at ${Date.now()}` }),
      { ...auth, tags: { name: 'POST /posts' } });
}
