import { bodyRequired, bodyTooLong, invalidPostId, malformedJson } from "./errors";

const MAX_SAFE_INT_STR = "9007199254740991";
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
  // Strip leading zeros ("0"/"00" is zero, not positive). Any stripped length
  // above 16 exceeds MAX_SAFE_INTEGER, so it can only be a 404 sentinel:
  // 17-19 digits are > 2^53 but <= SQLITE_MAX, 20+ exceed SQLITE_MAX too.
  let i = 0;
  while (i < raw.length && raw.charCodeAt(i) === 48) i++;
  if (i === raw.length) throw invalidPostId();
  const slen = raw.length - i;
  if (slen > 16) return MISSING_SENTINEL;
  if (slen === 16 && raw.slice(i) > MAX_SAFE_INT_STR) return MISSING_SENTINEL;
  return Number(raw);
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
