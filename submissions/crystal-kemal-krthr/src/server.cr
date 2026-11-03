require "kemal"
require "sqlite3"
require "base64"
require "openssl/hmac"
require "crypto/subtle"

# The driver's default text reader stops at NUL instead of SQLite's byte length.
class SQLite3::ResultSet
  def read(type : String.class) : String
    return super unless LibSQLite3.column_type(self, @column_index) == Type::TEXT
    moving_column do |column|
      String.new(LibSQLite3.column_text(self, column), LibSQLite3.column_bytes(self, column))
    end
  end
end

require "./app"

Kemal.config.host_binding = ENV.fetch("HOST", "0.0.0.0")
Kemal.config.port = ENV.fetch("PORT", "3000").to_i32
Kemal.config.env = "production"
Kemal.config.logging = false
Kemal.config.serve_static = false

Kemal.run
