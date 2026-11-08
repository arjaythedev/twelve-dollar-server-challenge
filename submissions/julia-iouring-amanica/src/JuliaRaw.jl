module JuliaRaw

# Dev-only instrumentation, fixed when the module is loaded (JR_DEBUG=1, set by dev/ct.sh --debug).
# With it off, every `@dbg expr` expands to `nothing`: the compiled server contains no debug code at all,
# not even a branch. (A future ahead-of-time build bakes the value in at build time.)
const DEBUG = get(ENV, "JR_DEBUG", "") == "1"
macro dbg(ex)
    DEBUG ? esc(ex) : nothing
end

include("buf.jl")
include("json.jl")
include("jwt.jl")
include("db.jl")
include("http.jl")
include("handlers.jl")
include("epoll.jl")
include("uring.jl")

function main()
    START[] = time()
    path = get(ENV, "SQLITE_PATH", "")
    isempty(path) && error("SQLITE_PATH is not set")
    init_jwt(get(ENV, "JWT_SECRET", "twelve-dollar-challenge"))
    db_open(path)
    host = get(ENV, "HOST", "0.0.0.0")
    port = parse(Int, get(ENV, "PORT", "80"))
    lfd = listen_tcp(host, port)
    println("memory: total=$(Sys.total_memory() >> 20)MB free=$(Sys.free_memory() >> 20)MB")
    # JR_TRANSPORT (from TRANSPORT in config.sh, exported by start.sh): uring, epoll, or auto = io_uring
    # when the kernel allows it (seccomp, old kernel), else epoll.
    want = get(ENV, "JR_TRANSPORT", "auto")
    U = nothing
    if want != "epoll"
        U = try
            uring_init(lfd)
        catch e
            want == "uring" && rethrow()
            println("io_uring unavailable ($(sprint(showerror, e))): using epoll")
            nothing
        end
    end
    println("julia-iouring-amanica julia=$(VERSION) backend=sql transport=$(U === nothing ? "epoll" : "uring") ",
        "debug=$(DEBUG ? 1 : 0) listening on $host:$port")
    flush(stdout)
    U === nothing ? serve(lfd) : serve_uring(U)
end

end # module
