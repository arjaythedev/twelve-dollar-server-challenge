// API-only build: keep TinyJS routing and pass Bun's Request/Response through directly.
import { createApplication } from "./.tinyjs/server/server.mjs";
import { readFileSync, readdirSync } from "node:fs";

const handle = createApplication({ scripts: [], styles: [] });
const hostname = process.env.HOST ?? "0.0.0.0";
let retireConnections = false;
let pressureTimer: ReturnType<typeof setInterval> | undefined;
if (process.platform === "linux" && !["127.0.0.1", "::1", "localhost"].includes(hostname)) {
  const softLimit = Number(/^Max open files\s+(\d+)/m.exec(readFileSync("/proc/self/limits", "utf8"))?.[1]);
  if (Number.isFinite(softLimit) && softLimit > 0) {
    pressureTimer = setInterval(() => {
      try {
        const descriptors = readdirSync("/proc/self/fd").length;
        // Leave accept headroom; hysteresis avoids toggling on every sample.
        retireConnections = descriptors >= softLimit * (retireConnections ? 0.5 : 0.75);
      } catch (error) {
        retireConnections = true;
        console.error("Cannot inspect descriptor pressure; retiring response connections", error);
      }
    }, 1000);
    pressureTimer.unref();
  }
}
const server = Bun.serve({
  hostname,
  port: Number(process.env.PORT ?? 80),
  idleTimeout: 120,
  async fetch(request) {
    const response = request.method === "TRACE" || request.method === "TRACK"
      ? new Response("Not Implemented", { status: 501 }) : await handle(request);
    // Tell the client before closing: a silent idle close loses non-retryable POSTs.
    if (retireConnections) response.headers.set("Connection", "close");
    return response;
  },
  error(error) {
    console.error(error);
    return new Response("Internal Server Error", {
      status: 500, headers: { "Content-Type": "text/plain; charset=utf-8",
        ...(retireConnections ? { Connection: "close" } : {}) },
    });
  },
});
console.log(`TinyJS listening on ${server.url.origin}`);
for (const signal of ["SIGINT", "SIGTERM"] as const) {
  process.once(signal, () => { clearInterval(pressureTimer); void server.stop(); });
}
