import type { HealthReport } from '@modules/health/domain/entities/healthReport'

export const presentHealth = ({ status, db, uptimeSeconds }: HealthReport) => ({
  status,
  db,
  uptime_s: uptimeSeconds,
})
