// API-only build: keep TinyJS routing and pass Bun's Request/Response through directly.
import { createApplication } from "./.tinyjs/server/server.mjs";

const handle = createApplication({ scripts: [], styles: [] });
const server = Bun.serve({
  hostname: process.env.HOST ?? "0.0.0.0",
  port: Number(process.env.PORT ?? 80),
  idleTimeout: 120,
  fetch(request) {
    if (request.method === "TRACE" || request.method === "TRACK") {
      return new Response("Not Implemented", { status: 501 });
    }
    return handle(request);
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
