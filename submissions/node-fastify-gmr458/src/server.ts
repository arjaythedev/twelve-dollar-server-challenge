import { buildApp } from "./app.js";
import { HOST, PORT } from "./config.js";

try {
  const app = buildApp();
  await app.listen({ host: HOST, port: PORT, backlog: 4096 });
} catch (err) {
  console.error(err);
  process.exit(1);
}
