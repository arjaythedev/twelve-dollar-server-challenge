// Execute the upstream workload unchanged, adding endpoint-specific summaries.
import workload, { options as upstreamOptions } from '../../../bench/load.js';

const endpoints = ['GET /feed', 'GET /posts/:id', 'POST /posts/:id/like', 'POST /posts'];
const thresholds = { ...upstreamOptions.thresholds };
for (const endpoint of endpoints) {
  // Non-restrictive thresholds cause k6 to retain these tag-filtered submetrics.
  thresholds[`http_req_duration{name:${endpoint}}`] = ['max>=0'];
  thresholds[`http_reqs{name:${endpoint}}`] = ['count>=0'];
  thresholds[`http_req_failed{name:${endpoint}}`] = ['rate<=1'];
}
export const options = { ...upstreamOptions, thresholds };
export default workload;

export function handleSummary(data) {
  data.local = { finished_utc: new Date().toISOString(), vus: Number(__ENV.VUS), hold: __ENV.DURATION, ramp: __ENV.RAMP_UP };
  const duration = data.metrics.http_req_duration.values;
  const requests = data.metrics.http_reqs.values;
  const failures = data.metrics.http_req_failed.values;
  return {
    [__ENV.RESULT_JSON]: JSON.stringify(data, null, 2),
    stdout: `requests=${requests.count} overall_rps=${requests.rate.toFixed(2)} ` +
      `p95_ms=${duration['p(95)'].toFixed(2)} p99_ms=${duration['p(99)'].toFixed(2)} ` +
      `failed_pct=${(failures.rate * 100).toFixed(3)}\n`,
  };
}
