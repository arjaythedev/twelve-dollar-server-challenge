export interface ServerConfig {
  sqlitePath: string;
  jwtSecret: Buffer;
  host: string;
  port: number;
  startedAtMs: number;
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
    startedAtMs: Date.now(),
  };
}
