const required = (name: string): string => {
  const value = process.env[name]
  if (!value) throw new Error(`${name} is not set`)
  return value
}

const positiveNumber = (name: string, fallback: number): number => {
  const raw = process.env[name]
  if (raw === undefined || raw === '') return fallback
  const value = Number(raw)
  if (!Number.isFinite(value) || value <= 0) throw new Error(`${name} must be a positive number`)
  return value
}

const ratio = (name: string, fallback: number): number => {
  const value = positiveNumber(name, fallback)
  if (value > 1) throw new Error(`${name} must be at most 1`)
  return value
}

export const env = {
  sqlitePath: required('SQLITE_PATH'),
  jwtSecret: required('JWT_SECRET'),
  host: process.env.HOST ?? '127.0.0.1',
  port: Number(process.env.PORT ?? 3000),
  memorySoftLimitRatio: ratio('MEMORY_SOFT_LIMIT_RATIO', 0.75),
  memoryCheckIntervalMs: positiveNumber('MEMORY_CHECK_INTERVAL_MS', 2_000),
  memoryGcCooldownMs: positiveNumber('MEMORY_GC_COOLDOWN_MS', 30_000),
} as const
