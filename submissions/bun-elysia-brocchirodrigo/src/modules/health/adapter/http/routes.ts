import { presentHealth } from '@modules/health/adapter/http/healthPresenter'
import type { CheckHealth } from '@modules/health/application/useCases/checkHealth'
import { DatabaseUnreachableError } from '@modules/health/domain/exceptions/databaseUnreachableError'
import { Elysia, status } from 'elysia'

export const healthRoutes = (checkHealth: CheckHealth) =>
  new Elysia({ name: 'health' }).get('/health', () => {
    const report = checkHealth()
    return report instanceof DatabaseUnreachableError
      ? status(503, { status: 'degraded', db: 'unreachable', error: report.message })
      : presentHealth(report)
  })
