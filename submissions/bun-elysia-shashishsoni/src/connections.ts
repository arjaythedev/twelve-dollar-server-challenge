import { readFileSync, statSync } from "node:fs";

const SHED_FROM = 0.85;
const SHED_FULL = 0.95;
const SAMPLE_INTERVAL_MS = 50;

let shedProbability = 0;

export function shedForLoad(openFds: number, limit: number): number {
  if (limit <= 0) return 0;
  const frac = openFds / limit;
  if (frac <= SHED_FROM) return 0;
  if (frac >= SHED_FULL) return 1;
  return (frac - SHED_FROM) / (SHED_FULL - SHED_FROM);
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
    shedProbability = shedForLoad(openDescriptors(), limit);
  }, SAMPLE_INTERVAL_MS);
}

export function parseOpenFilesLimit(limitsFile: string): number {
  const match = /^Max open files\s+(\d+)/m.exec(limitsFile);
  return match ? Number(match[1]) : 0;
}

export function shedProbabilityNow(): number {
  return shedProbability;
}
