import { bodyRequired, bodyTooLong, invalidPostId, malformedJson } from "./errors";

const MAX_SQLITE_INT = 9223372036854775807n;
const MISSING_SENTINEL = Number.MAX_SAFE_INTEGER + 1;

function isDigits(value: string): boolean {
  if (value.length === 0) return false;
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code < 48 || code > 57) return false;
  }
  return true;
}

export function parsePostId(raw: string): number {
  if (!isDigits(raw)) throw invalidPostId();
  const asBig = BigInt(raw);
  if (asBig <= 0n) throw invalidPostId();
  if (asBig > MAX_SQLITE_INT) return MISSING_SENTINEL;
  if (asBig > BigInt(Number.MAX_SAFE_INTEGER)) return MISSING_SENTINEL;
  return Number(asBig);
}

export function isMissingSentinel(id: number): boolean {
  return id === MISSING_SENTINEL;
}

export async function readJsonBody(request: Request): Promise<unknown> {
  try {
    return await request.json();
  } catch {
    throw malformedJson();
  }
}

export function extractTrimmedBody(payload: unknown): string {
  const body = typeof payload === "object" && payload !== null
    ? (payload as Record<string, unknown>).body
    : undefined;
  if (typeof body !== "string") throw bodyRequired();
  const trimmed = body.trim();
  if (trimmed.length === 0) throw bodyRequired();
  if (trimmed.length > 500) throw bodyTooLong();
  return trimmed;
}
