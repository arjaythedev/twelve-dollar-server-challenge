using System.Text;
using Feed;

// Configuration comes from environment variables only. SQLITE_PATH and JWT_SECRET are
// required; HOST defaults to 127.0.0.1 and PORT to 3000.
string sqlitePath = Environment.GetEnvironmentVariable("SQLITE_PATH")
    ?? throw new InvalidOperationException("SQLITE_PATH is not set");
string secret = Environment.GetEnvironmentVariable("JWT_SECRET")
    ?? throw new InvalidOperationException("JWT_SECRET is not set");
string host = Environment.GetEnvironmentVariable("HOST") ?? "127.0.0.1";
if (!int.TryParse(Environment.GetEnvironmentVariable("PORT"), out int port)) {
    port = 3000;
}

Db db = new(sqlitePath);
Api api = new(db, new Jwt(Encoding.UTF8.GetBytes(secret)));
Server server = new(host, port, api, db);
Console.WriteLine($"listening on {host}:{port}");
server.Run();
