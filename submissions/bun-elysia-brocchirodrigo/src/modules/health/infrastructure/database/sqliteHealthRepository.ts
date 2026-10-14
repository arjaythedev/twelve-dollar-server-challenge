import type { Database } from 'bun:sqlite'
import type { HealthRepository } from '@modules/health/domain/repositories/healthRepository'

export const createSqliteHealthRepository = (db: Database): HealthRepository => {
  const ping = db.query<{ ok: number }, []>('SELECT 1 AS ok')

  return {
    ping: () => {
      ping.get()
    },
  }
}
