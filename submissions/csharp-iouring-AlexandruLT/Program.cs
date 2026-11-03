using System.Diagnostics;
using System.Runtime.InteropServices;
using Feed;

// 8 rings x 60,000 registered slots: connections need no ordinary file descriptor, and each
// ring's slot table stays under the 65,535 descriptor limit the box enforces.
const int Rings = 8, Slots = 60000, WriteBatchSize = 256, WriteWindowMs = 20, CheckpointPages = 4000;

string Required(string name) => Environment.GetEnvironmentVariable(name) ?? throw new InvalidOperationException(name + " is required");
string dbPath = Required("SQLITE_PATH");
var jwt = new Jwt(Required("JWT_SECRET"));
string host = Environment.GetEnvironmentVariable("HOST") ?? "0.0.0.0";
int port = int.Parse(Environment.GetEnvironmentVariable("PORT") ?? "80");
if (!UringServer.Available(Slots))
{
    Console.Error.WriteLine($"io_uring with SINGLE_ISSUER|DEFER_TASKRUN (Linux 6.1+) and RLIMIT_NOFILE >= {Slots} are required");
    return 1;
}

long started = Stopwatch.GetTimestamp();
using var db = new Db(dbPath);
using var checkpoints = new BackgroundCheckpoint(dbPath, db, CheckpointPages);
using var server = new UringServer(db, jwt, started, Rings, Slots, WriteBatchSize, WriteWindowMs);
using var term = PosixSignalRegistration.Create(PosixSignal.SIGTERM, context => { context.Cancel = true; server.Stop(); });
using var interrupt = PosixSignalRegistration.Create(PosixSignal.SIGINT, context => { context.Cancel = true; server.Stop(); });
server.Run(host, port);
return 0;
