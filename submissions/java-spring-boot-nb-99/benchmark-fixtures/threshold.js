// Captured with the pinned k6 image; emits exit 99 without loading the server.
import { Counter } from 'k6/metrics';
const count = new Counter('contract_count');
export const options = { vus: 1, iterations: 1, thresholds: { contract_count: ['count<1'] } };
export default function () { count.add(1); }
