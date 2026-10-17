import '@server/runtime/production'
import { api } from '@server/api'
import { env } from '@server/config/env'
import { startMemoryGuard } from '@server/runtime/memoryGuard'

api.listen({ hostname: env.host, port: env.port, idleTimeout: 120 })
startMemoryGuard({
  softLimitRatio: env.memorySoftLimitRatio,
  checkIntervalMs: env.memoryCheckIntervalMs,
  gcCooldownMs: env.memoryGcCooldownMs,
})

console.log(`listening on http://${env.host}:${env.port}`)
