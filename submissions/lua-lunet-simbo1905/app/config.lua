---The application configuration, read once from the SPEC's env vars at load.
---@class Config
---@field SQLITE_PATH string The SQLite database file path
---@field JWT_SECRET string The HS256 signing secret
---@field HOST string The listen host
---@field PORT number The listen port
local config = {}

config.SQLITE_PATH = os.getenv("SQLITE_PATH") or "./feed.db"
config.JWT_SECRET = os.getenv("JWT_SECRET") or "twelve-dollar-challenge"
config.HOST = os.getenv("HOST") or "127.0.0.1"
config.PORT = tonumber(os.getenv("PORT")) or 3000

return config
