export interface ServerConfig {
  sqlitePath: string;
  jwtSecret: Buffer;
  host: string;
  port: number;
  idleTimeoutSec: number;
  startedAtMs: number;
}

// Bun sweeps idle sockets every 4 s, so N seconds really closes after 4*ceil(N/4)-4 .. 4*ceil(N/4) s.
// 13 => 12-16 s: never during the <= 8 s pause before a like/post, which clients don't retry.
const NGINX_IDLE_TIMEOUT_SEC = 70;
const DIRECT_IDLE_TIMEOUT_SEC = 13;

function isLoopback(host: string): boolean {
  return host === "127.0.0.1" || host === "localhost" || host === "::1";
}

function required(name: string, fallback?: string): string {
  const value = Bun.env[name] ?? process.env[name] ?? fallback;
  if (!value) throw new Error(`missing env ${name}`);
  return value;
}

export function loadConfig(): ServerConfig {
  const sqlitePath = required("SQLITE_PATH");
  const secret = required("JWT_SECRET");
  const host = required("HOST", "127.0.0.1");
  const port = Number(required("PORT", "3000"));
  if (!Number.isInteger(port) || port <= 0 || port > 65535) {
    throw new Error(`invalid PORT: ${port}`);
  }
  return {
    sqlitePath,
    jwtSecret: Buffer.from(secret, "utf8"),
    host,
    port,
    idleTimeoutSec: isLoopback(host) ? NGINX_IDLE_TIMEOUT_SEC : DIRECT_IDLE_TIMEOUT_SEC,
    startedAtMs: Date.now(),
  };
}
