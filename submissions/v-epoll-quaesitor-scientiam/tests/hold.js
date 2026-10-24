// A denser variant of bench/load.js for memory and allocation checks: every VU keeps one
// connection open and hits all routes with short think times. Not a scoring workload.
import http from 'k6/http';
import { check, sleep } from 'k6';
import { SharedArray } from 'k6/data';

const BASE = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const VUS = Number(__ENV.VUS || 1000);
const DURATION = __ENV.DURATION || '2m';
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS || '../../../seed/tokens.json')));

export const options = {
  scenarios: {
    hold: { executor: 'ramping-vus', stages: [{ duration: '20s', target: VUS }, { duration: DURATION, target: VUS }, { duration: '5s', target: 0 }] },
  },
  thresholds: { http_req_failed: ['rate<0.01'] },
};

export default function () {
  const token = tokens[(__VU - 1) % tokens.length].token;
  const auth = { headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' } };
  const feed = http.get(`${BASE}/feed`);
  check(feed, { 'feed 200': (r) => r.status === 200 });
  let id = 1;
  try { id = JSON.parse(feed.body).posts[Math.floor(Math.random() * 20)].id; } catch (e) {}
  sleep(0.2 + Math.random() * 0.3);
  const post = http.get(`${BASE}/posts/${id}`);
  check(post, { 'post 200': (r) => r.status === 200 });
  if (Math.random() < 0.15) {
    const like = http.post(`${BASE}/posts/${id}/like`, null, auth);
    check(like, { 'like 2xx': (r) => r.status === 200 || r.status === 201 });
  }
  if (Math.random() < 0.02) {
    const c = http.post(`${BASE}/posts`, JSON.stringify({ body: `hold ${__VU} ${__ITER}` }), auth);
    check(c, { 'create 201': (r) => r.status === 201 });
  }
  sleep(0.5 + Math.random() * 0.5);
}
