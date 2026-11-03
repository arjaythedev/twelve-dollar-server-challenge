// Saturation probe (tbsvttr mixed.lua distribution, no think time):
// 100 feed : 100 post : 15 like : 2 create over 64 conns, for raw req/s.
// k6 run --vus 64 --duration 15s dev/saturation.js (TOKENS env for seed tokens)
import http from 'k6/http';
import { SharedArray } from 'k6/data';
const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS || '../seed/tokens.json')));
let ids = [500000];
let serial = 0;
export const options = { summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'] };
export default function () {
  const r = Math.floor(Math.random() * 217);
  if (r < 100) {
    const res = http.get(`${BASE_URL}/feed`);
    if (res.status === 200) {
      try {
        const arr = res.json('posts');
        if (arr && arr.length) ids = arr.map((p) => p.id);
      } catch (e) {}
    }
    return;
  }
  const id = ids[Math.floor(Math.random() * ids.length)];
  if (r < 200) { http.get(`${BASE_URL}/posts/${id}`); return; }
  const user = tokens[Math.floor(Math.random() * tokens.length)];
  const auth = { headers: { Authorization: `Bearer ${user.token}`, 'Content-Type': 'application/json' } };
  if (r < 215) { http.post(`${BASE_URL}/posts/${id}/like`, null, auth); return; }
  serial++;
  http.post(`${BASE_URL}/posts`, JSON.stringify({ body: `sat probe post ${serial}` }), auth);
}
