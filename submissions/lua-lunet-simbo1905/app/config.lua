local config = {}

config.SQLITE_PATH = os.getenv("SQLITE_PATH") or "./feed.db"
config.JWT_SECRET = os.getenv("JWT_SECRET") or "twelve-dollar-challenge"
config.HOST = os.getenv("HOST") or "127.0.0.1"
config.PORT = tonumber(os.getenv("PORT")) or 3000

return config
