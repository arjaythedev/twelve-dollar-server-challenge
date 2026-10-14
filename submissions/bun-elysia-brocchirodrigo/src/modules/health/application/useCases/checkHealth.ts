import type { HealthReport } from '@modules/health/domain/entities/healthReport'
import { DatabaseUnreachableError } from '@modules/health/domain/exceptions/databaseUnreachableError'
import type { HealthRepository } from '@modules/health/domain/repositories/healthRepository'

export type CheckHealth = () => HealthReport | DatabaseUnreachableError

export const makeCheckHealth = (repository: HealthRepository): CheckHealth => {
  const startedAt = Date.now()

  return () => {
    try {
      repository.ping()
    } catch (error) {
      return new DatabaseUnreachableError(error instanceof Error ? error.message : String(error))
    }
    return { status: 'ok', db: 'ok', uptimeSeconds: Math.floor((Date.now() - startedAt) / 1000) }
  }
}
