// HTTP cases test/test.sh doesn't cover: split and pipelined requests, backpressure, Connection: close.
// Usage: bun check.ts [port], against a server on a fresh copy of seed/feed.db.
import type { Socket } from "bun";

const PORT = Number(process.argv[2] ?? 3000);
const SECRET = "twelve-dollar-challenge";
let failed = 0;

function ok(cond: boolean, label: string) {
  console.log((cond ? "  ok   " : "  FAIL ") + label);
  if (!cond) failed++;
}

type Response = [status: number, body: string];

// Raw HTTP client; pause() stops reading so the server's writes back up.
class Client {
  private buf = Buffer.alloc(0);
  private pending: Buffer | null = null;
  private wake: (() => void) | null = null;
  closed = false;
  private constructor(private sock: Socket<Client>) {}

  static async connect(): Promise<Client> {
    const sock = await Bun.connect<Client>({
      hostname: "127.0.0.1",
      port: PORT,
      socket: {
        data(s, d) {
          s.data.buf = Buffer.concat([s.data.buf, d]);
          s.data.wake?.();
        },
        drain(s) {
          const c = s.data;
          if (c.pending) {
            const n = s.write(c.pending);
            c.pending = n < c.pending.length ? c.pending.subarray(n) : null;
          }
        },
        close(s) {
          s.data.closed = true;
          s.data.wake?.();
        },
      },
    });
    sock.data = new Client(sock);
    return sock.data;
  }

  send(bytes: string | Buffer) {
    const b = Buffer.from(bytes);
    if (this.pending) return void (this.pending = Buffer.concat([this.pending, b]));
    const n = this.sock.write(b);
    if (n < b.length) this.pending = b.subarray(Math.max(n, 0));
  }

  pause() { this.sock.pause(); }
  resume() { this.sock.resume(); }

  async read(n: number): Promise<Response[]> {
    const out: Response[] = [];
    const deadline = Date.now() + 5000;
    while (out.length < n) {
      const headEnd = this.buf.indexOf("\r\n\r\n");
      if (headEnd >= 0) {
        const head = this.buf.toString("latin1", 0, headEnd);
        const len = Number(/content-length: *(\d+)/i.exec(head)![1]);
        if (this.buf.length >= headEnd + 4 + len) {
          out.push([Number(head.split(" ")[1]), this.buf.toString("utf8", headEnd + 4, headEnd + 4 + len)]);
          this.buf = this.buf.subarray(headEnd + 4 + len);
          continue;
        }
      }
      if (this.closed || Date.now() > deadline) break;
      await Promise.race([new Promise<void>((r) => (this.wake = r)), Bun.sleep(deadline - Date.now())]);
    }
    return out;
  }

  async closedByServer(): Promise<boolean> {
    const deadline = Date.now() + 1000;
    while (!this.closed && Date.now() < deadline) await Promise.race([new Promise<void>((r) => (this.wake = r)), Bun.sleep(50)]);
    return this.closed && this.buf.length === 0;
  }
}

const b64 = (s: string) => Buffer.from(s).toString("base64url");
function token(header: unknown, payload: unknown): string {
  const signing = `${b64(JSON.stringify(header))}.${b64(JSON.stringify(payload))}`;
  return `${signing}.${new Bun.CryptoHasher("sha256", SECRET).update(signing).digest("base64url")}`;
}

function req(method: string, path: string, auth?: string, body?: string): string {
  let r = `${method} ${path} HTTP/1.1\r\nHost: x\r\n`;
  if (auth) r += `Authorization: Bearer ${auth}\r\n`;
  if (body !== undefined) r += `Content-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\n`;
  return r + "\r\n" + (body ?? "");
}

const statuses = (rs: Response[]) => JSON.stringify(rs.map((r) => r[0]));
const now = Math.floor(Date.now() / 1000);
const TOK = token({ alg: "HS256", typ: "JWT" }, { sub: "7", username: "u7", iat: now, exp: now + 3600 });

let c = await Client.connect();
c.send(req("GET", "/feed"));
const FEED = (await c.read(1))[0][1];

console.log("-- framing");
c = await Client.connect();
const one = req("GET", "/feed");
for (let i = 0; i < one.length; i++) {
  c.send(one[i]);
  await Bun.sleep(1);
}
let rs = await c.read(1);
ok(rs.length === 1 && rs[0][0] === 200 && rs[0][1] === FEED, "request split byte by byte");

const three = req("GET", "/feed") + req("GET", "/posts/1") + req("GET", "/zzz");
c.send(three);
rs = await c.read(3);
ok(statuses(rs) === "[200,200,404]" && rs[0][1] === FEED, "3 pipelined requests in one write, same connection");
c.send(three.slice(0, 40));
await Bun.sleep(50);
c.send(three.slice(40));
ok(statuses(await c.read(3)) === "[200,200,404]", "pipelined requests split mid-request");

c = await Client.connect();
const like = req("POST", "/posts/2/like", TOK, '{"x":1}').replace("Authorization", "aUtHoRiZaTiOn").replace("Content-Length", "content-length");
c.send(like.slice(0, -4));
await Bun.sleep(50);
c.send(like.slice(-4));
rs = await c.read(1);
ok(rs.length === 1 && (rs[0][0] === 200 || rs[0][0] === 201), "POST with mixed-case headers and the body split across writes");

c = await Client.connect();
c.send("GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
rs = await c.read(1);
ok(rs.length === 1 && rs[0][0] === 200 && (await c.closedByServer()), "Connection: close -> response, then the server closes");

c = await Client.connect();
c.pause(); // ~5 MB unread forces short writes on the server
c.send(req("GET", "/feed").repeat(1000));
await Bun.sleep(1000);
c.resume();
rs = await c.read(1000);
ok(rs.length === 1000 && rs.every((r) => r[0] === 200 && r[1] === FEED), "1000 pipelined feeds against a paused client (backpressure)");

c = await Client.connect();
c.send("GET /" + "a".repeat(20000));
ok(await c.closedByServer(), "request head over 16 KB -> connection closed");

console.log("-- body length counts characters");
c = await Client.connect();
c.send(req("POST", "/posts", TOK, JSON.stringify({ body: "😀".repeat(500) })));
rs = await c.read(1);
ok(rs[0]?.[0] === 201 && JSON.parse(rs[0][1]).post.body === "😀".repeat(500), "500 emoji (1000 UTF-16 units) -> 201");
c.send(req("POST", "/posts", TOK, JSON.stringify({ body: "😀".repeat(501) })));
rs = await c.read(1);
ok(rs[0]?.[0] === 400 && rs[0][1] === '{"error":"body must be at most 500 characters"}', "501 emoji -> 400");

console.log("-- JWT header variants");
for (const [label, header, want] of [
  ["keys in another order", { typ: "JWT", alg: "HS256" }, 201],
  ["HS512 header", { alg: "HS512", typ: "JWT" }, 401],
  ["header is not an object", "HS256", 401],
] as const) {
  c = await Client.connect();
  c.send(req("POST", "/posts", token(header, { sub: "7", username: "u7", exp: now + 60 }), '{"body":"hi"}'));
  ok((await c.read(1))[0]?.[0] === want, `${label} -> ${want}`);
}

console.log(failed === 0 ? "passed" : `failed=${failed}`);
process.exit(failed === 0 ? 0 : 1);
