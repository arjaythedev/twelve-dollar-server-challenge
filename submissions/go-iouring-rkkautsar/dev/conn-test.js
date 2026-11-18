import http from 'k6/http';
import { sleep } from 'k6';

// Connection-only probe: same think time, no SQLite work beyond SELECT 1.
export const options = {
  scenarios: {
    conns: {
      executor: 'ramping-vus',
      stages: [
        { duration: __ENV.RAMP_UP || '60s', target: Number(__ENV.VUS || 10000) },
        { duration: __ENV.DURATION || '30s', target: Number(__ENV.VUS || 10000) },
        { duration: '10s', target: 0 },
      ],
      gracefulRampDown: '5s',
    },
  },
  summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'],
};

export default function () {
  http.get(`${__ENV.BASE_URL || 'http://127.0.0.1:3000'}/health`);
  sleep(10 + Math.random() * 5);
}
