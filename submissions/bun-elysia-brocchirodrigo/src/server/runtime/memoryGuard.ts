type MemoryGuardOptions = {
  softLimitRatio: number
  checkIntervalMs: number
  gcCooldownMs: number
}

export const startMemoryGuard = ({ softLimitRatio, checkIntervalMs, gcCooldownMs }: MemoryGuardOptions): void => {
  const softLimit = Math.floor(process.constrainedMemory() * softLimitRatio)
  let lastGcAt = 0

  setInterval(() => {
    const now = Date.now()
    if (now - lastGcAt < gcCooldownMs || process.memoryUsage.rss() <= softLimit) return
    lastGcAt = now
    Bun.gc(true)
  }, checkIntervalMs).unref()
}
