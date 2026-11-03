import { readFileSync, readdirSync } from "node:fs";

// Sample Linux process descriptors; Bun.serve does not expose an HTTP connection count.
let budget = 60000;
try {
  const limit = readFileSync("/proc/self/limits", "utf8").match(/^Max open files\s+(\d+)/m);
  if (limit) budget = Math.max(1, Math.min(budget, Number(limit[1]) - 128));
} catch {} // /proc is unavailable on macOS; ordinary keep-alive still works there.
let nextSample = 0, retiring = false;

export function connectionPressure(): boolean {
  const now = performance.now();
  if (now >= nextSample) {
    nextSample = now + 100;
    try { retiring = readdirSync("/proc/self/fd").length >= budget; }
    catch (error) {
      if (error instanceof Error && "code" in error &&
          (error.code === "EMFILE" || error.code === "ENFILE")) retiring = true;
    }
  }
  return retiring;
}
