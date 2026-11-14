from typing import Optional

import apsw
import apsw.aio  # enables async controllers

DB_PATH: str = ""
db: Optional[apsw.Connection] = None


async def init_db(path: str):
    """Opens the async connection and applies pragmas."""
    global DB_PATH, db
    DB_PATH = path

    # Create async connection
    db = await apsw.Connection.as_async(path)

    # Apply pragmas
    await db.execute("PRAGMA journal_mode=WAL")
    await db.execute("PRAGMA synchronous=NORMAL")
    await db.execute("PRAGMA busy_timeout=5000")
    await db.execute("PRAGMA cache_size=-64000")  # 64 MiB
    await db.execute("PRAGMA mmap_size=1073741824")  # 1 GiB
    await db.execute("PRAGMA locking_mode=EXCLUSIVE")
    await db.execute("PRAGMA foreign_keys=ON")

    # Warm cache
    await db.execute("SELECT count(*) FROM posts")


async def close_db():
    """Closes the connection."""
    global db
    if db:
        await db.close()
        db = None


async def fetchone(cursor):
    """Returns the first row of the cursor or None."""
    async for row in cursor:
        return row
    return None


async def fetchall(cursor):
    """Returns all cursor rows as a list."""
    return [row async for row in cursor]
