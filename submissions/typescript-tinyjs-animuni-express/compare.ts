import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

// Linux-only local transport probe; run after building, against a disposable seeded DB.
assert.equal(process.platform, "linux");
assert.ok(process.env.SQLITE_PATH, "SQLITE_PATH must point to a disposable seeded database");
const ticks = Number(Bun.spawnSync(["getconf", "CLK_TCK"]).stdout.toString());
assert.ok(ticks > 0);
const cpu = async (pid: number) => {
  const stat = (await readFile(`/proc/${pid}/stat`, "utf8")).split(") ")[1].split(" ");
  return (Number(stat[11]) + Number(stat[12])) / ticks;
};
for (const transport of ["node", "bun", "bun", "node"]) {
  const entry = transport === "bun" ? ["transport.ts"] :
    ["node_modules/tinyjs-fullstack/bin/tinyjs.js", "serve", "--host", "127.0.0.1", "--port", "0"];
  const child = Bun.spawn([process.execPath, ...entry], {
    env: { ...process.env, HOST: "127.0.0.1", PORT: "0" }, stdout: "pipe", stderr: "inherit",
  });
  try {
    const url = await Promise.race([
      (async () => {
        let output = "";
        for await (const chunk of child.stdout) {
          output += new TextDecoder().decode(chunk);
          const match = /http:\/\/127\.0\.0\.1:\d+/.exec(output);
          if (match) return match[0];
        }
        throw new Error("server exited before listening");
      })(),
      new Promise<never>((_, reject) => setTimeout(() => reject(new Error("startup timeout")), 10000).unref()),
    ]);
    const paths = ["/feed", "/posts/1", "/posts/2", "/posts/3"];
    const get = async (index: number) => {
      const response = await fetch(url + paths[index % paths.length]);
      assert.equal(response.status, 200);
      await response.arrayBuffer();
    };
    for (let index = 0; index < 200; index++) await get(index);
    const before = await cpu(child.pid);
    const started = performance.now(), end = started + 5000, latency: number[] = [];
    await Promise.all(Array.from({ length: 16 }, async (_, worker) => {
      let index = worker;
      while (performance.now() < end) {
        const start = performance.now();
        await get(index++);
        latency.push(performance.now() - start);
      }
    }));
    const seconds = (performance.now() - started) / 1000;
    const cpuSeconds = await cpu(child.pid) - before;
    const status = await readFile(`/proc/${child.pid}/status`, "utf8");
    latency.sort((a, b) => a - b);
    assert.ok(latency.length > 0);
    console.log(JSON.stringify({ transport, requests: latency.length, seconds,
      rps: latency.length / seconds, cpuSeconds, cpuMicrosecondsPerRequest: cpuSeconds * 1e6 / latency.length,
      p95ms: latency[Math.ceil(latency.length * .95) - 1], p99ms: latency[Math.ceil(latency.length * .99) - 1],
      peakRssKiB: Number(/VmHWM:\s+(\d+)/.exec(status)?.[1]) }));
  } finally {
    child.kill();
    await child.exited;
  }
}
