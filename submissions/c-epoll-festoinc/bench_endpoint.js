// Per-endpoint stress test for local profiling. Not used for scoring.
//   MODE=feed|post|like|create k6 run -e MODE=feed -e VUS=100 -e DURATION=10s bench_endpoint.js
import http from 'k6/http';
import { SharedArray } from 'k6/data';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const MODE = __ENV.MODE || 'feed';
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS || '../../seed/tokens.json')));
const VUS = Number(__ENV.VUS || 100);
const DURATION = __ENV.DURATION || '10s';

export const options = {
  scenarios: { blast: { executor: 'constant-vus', vus: VUS, duration: DURATION } },
  summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'],
};

export default function () {
  const user = tokens[(__VU - 1) % tokens.length];
  const auth = { headers: { Authorization: `Bearer ${user.token}`, 'Content-Type': 'application/json' } };
  const id = MODE.endsWith('top') ? 500000 : (1 + Math.floor(Math.random() * 500000));
  if (MODE === 'health') {
    http.get(`${BASE_URL}/health`, { tags: { name: 'health' } });
  } else if (MODE === 'feed') {
    http.get(`${BASE_URL}/feed`, { tags: { name: 'feed' } });
  } else if (MODE === 'post' || MODE === 'posttop') {
    http.get(`${BASE_URL}/posts/${id}`, { tags: { name: 'post' } });
  } else if (MODE === 'like' || MODE === 'liketop') {
    http.post(`${BASE_URL}/posts/${id}/like`, null, { ...auth, tags: { name: 'like' } });
  } else if (MODE === 'create') {
    http.post(`${BASE_URL}/posts`, JSON.stringify({ body: `${user.username} ${Date.now()}` }),
      { ...auth, tags: { name: 'create' } });
  }
}
