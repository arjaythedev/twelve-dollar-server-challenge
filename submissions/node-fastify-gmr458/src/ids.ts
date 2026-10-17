export const MAX_ID = Number.MAX_SAFE_INTEGER;

export function parseId(raw: string): number | null {
  if (raw.length === 0 || raw.length > 20) return null;
  for (let i = 0; i < raw.length; i++) {
    const c = raw.charCodeAt(i);
    if (c < 48 || c > 57) return null;
  }
  const n = Number(raw);
  if (!(n > 0)) return null;
  return n;
}
