// One thread, io_uring, SQLite. See README.md for the design.
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

/// The one App the C event loop calls into.
var appInstance: App! = nil

/// Called by the event loop for the request at the start of `buf`. See lp_handler in shim.h.
let requestHandler: lp_handler = { buf, len, out, used in
    var req = Request()
    let outw = Out(o: out!)
    switch parseRequest(Slice(p: buf!, n: Int(len)), &req) {
    case .done(let n):
        if req.method.equals("POST") && td_writes_held() != 0 { return 2 }
        appInstance.handle(req, outw)
        used!.pointee = UInt32(truncatingIfNeeded: n)
        return 1
    case .partial:
        return 0
    case .bad:
        writeError(outw, 400, "bad request")
        return -1
    }
}

func env(_ key: String, _ fallback: String) -> String {
    if let v = getenv(key) { return String(cString: v) }
    return fallback
}

func fail(_ msg: String) -> Never {
    fputs(msg + "\n", stderr)
    exit(1)
}

let path = env("SQLITE_PATH", "feed.db")
var secret = env("JWT_SECRET", "")
let host = env("HOST", "127.0.0.1")
guard let port = Int32(env("PORT", "3000")) else { fail("PORT must be a number") }

let db: Db
do { db = try Db(path: path) } catch let e as Db.OpenError { fail("database: \(e.message)") } catch { fail("database: \(error)") }
secret.withUTF8 { appInstance = App(db: db, secret: Slice(p: $0.baseAddress ?? UnsafePointer(emptyBytes), n: $0.count)) }

_ = lp_run(host, port, requestHandler)
exit(1)
