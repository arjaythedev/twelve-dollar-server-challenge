// API-only build: keep TinyJS routing and pass Bun's Request/Response through directly.
import { createApplication } from "./.tinyjs/server/server.mjs";

const handle = createApplication({ scripts: [], styles: [] });
const hostname = process.env.HOST ?? "0.0.0.0";
const idleTimeout = ["127.0.0.1", "::1", "localhost"].includes(hostname) ? 120 : 3;
const server = Bun.serve({
  hostname,
  port: Number(process.env.PORT ?? 80),
  // Reclaim idle direct sockets; loopback proxies need at least 65 seconds.
  idleTimeout,
  async fetch(request, server) {
    server.timeout(request, 120);
    try {
      if (request.method === "TRACE" || request.method === "TRACK") {
        return new Response("Not Implemented", { status: 501 });
      }
      return await handle(request);
    } finally {
      // Bun's request timeout persists on the keep-alive socket.
      server.timeout(request, idleTimeout);
    }
  },
  error(error) {
    console.error(error);
    return new Response("Internal Server Error", {
      status: 500, headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  },
});
console.log(`TinyJS listening on ${server.url.origin}`);
for (const signal of ["SIGINT", "SIGTERM"] as const) {
  process.once(signal, () => { void server.stop(); });
}
