import { readFileSync, statSync } from "node:fs";

// Near the descriptor limit, finished responses close their connection (server side, so the
// TIME_WAIT stays here) instead of idling in keep-alive; clients reconnect on their next request.
const RETIRE_ABOVE = 0.92;
const RESUME_BELOW = 0.88;
const SAMPLE_INTERVAL_MS = 250;

let retiring = false;

export function shouldRetire(openFds: number, limit: number, current: boolean): boolean {
  if (openFds > limit * RETIRE_ABOVE) return true;
  if (openFds < limit * RESUME_BELOW) return false;
  return current;
}

export function parseOpenFilesLimit(limitsFile: string): number {
  const match = /^Max open files\s+(\d+)/m.exec(limitsFile);
  return match ? Number(match[1]) : 0;
}

// Linux >= 6.2 reports the number of open descriptors as the size of /proc/self/fd (O(1)).
function openDescriptors(): number {
  try {
    return statSync("/proc/self/fd").size;
  } catch {
    return 0;
  }
}

export function startDescriptorWatch(): void {
  let limit = 0;
  try {
    limit = parseOpenFilesLimit(readFileSync("/proc/self/limits", "utf8"));
  } catch {
    return;
  }
  if (limit <= 0) return;
  setInterval(() => {
    retiring = shouldRetire(openDescriptors(), limit, retiring);
  }, SAMPLE_INTERVAL_MS);
}

export function isRetiringConnections(): boolean {
  return retiring;
}
