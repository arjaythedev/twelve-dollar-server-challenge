import { Database } from 'bun:sqlite'

const pragmas = [
  'PRAGMA journal_mode = WAL',
  'PRAGMA synchronous = NORMAL',
  'PRAGMA busy_timeout = 5000',
  'PRAGMA cache_size = -65536',
  'PRAGMA mmap_size = 1073741824',
  'PRAGMA temp_store = MEMORY',
]

export const openDatabase = (path: string): Database => {
  const db = new Database(path, { strict: true })
  for (const pragma of pragmas) db.run(pragma)
  return db
}
