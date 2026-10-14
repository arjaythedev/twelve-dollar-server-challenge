import { Database } from "bun:sqlite";

declare const self: Worker;

const CHECKPOINT_INTERVAL_MS = 1000;

self.onmessage = (event: MessageEvent<string>) => {
  const db = new Database(event.data, { create: false, readwrite: true });
  const checkpoint = db.query(`PRAGMA wal_checkpoint(PASSIVE)`);
  setInterval(() => {
    try {
      checkpoint.get();
    } catch (err) {
      console.error("checkpoint failed:", err);
    }
  }, CHECKPOINT_INTERVAL_MS);
};
