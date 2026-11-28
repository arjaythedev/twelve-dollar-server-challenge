module JuliaRaw

# Dev-only instrumentation, fixed when the module is loaded (JR_DEBUG=1, set by dev/ct.sh --debug).
# With it off, every `@dbg expr` expands to `nothing`: the compiled server contains no debug code at all,
# not even a branch. (The ahead-of-time build, src/app.jl, bakes the value in at build time.)
const DEBUG = get(ENV, "JR_DEBUG", "") == "1"
const AOT = Base.JLOptions().trim != 0      # true in the build made by src/app.jl (fixed at build time)
const VERSION_STR = string(VERSION)
macro dbg(ex)
    DEBUG ? esc(ex) : nothing
end

# A log line straight to fd 1 (unbuffered, so nothing to flush): `stdout` is typed as an abstract IO,
# which the trimmed build can't dispatch on.
function logln(s::String)
    t = s * "\n"
    GC.@preserve t ccall(:write, Cssize_t, (Cint, Ptr{UInt8}, Csize_t), 1, t, sizeof(t))
    nothing
end

include("prof.jl")
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
    logln("memory: total=$(Sys.total_memory() >> 20)MB free=$(Sys.free_memory() >> 20)MB")
    # JR_TRANSPORT (from TRANSPORT in config.sh, exported by start.sh): uring, epoll, or auto = io_uring
    # when the kernel allows it (seccomp, old kernel), else epoll.
    want = get(ENV, "JR_TRANSPORT", "auto")
    U = nothing
    if want != "epoll"
        U = try
            uring_init(lfd)
        catch e
            want == "uring" && rethrow()
            msg = e isa ErrorException && e.msg isa String ? e.msg::String : "setup failed"
            logln("io_uring unavailable ($msg): using epoll")
            nothing
        end
    end
    writer = U !== nothing && w_start(path)
    ckpt = U !== nothing && ckpt_start(path)
    # built in steps: one long string(...) or a*b*c*... call widens to Any (not trimmable)
    s = "julia-iouring-amanica julia=" * VERSION_STR
    s *= AOT ? " build=aot backend=sql " : " build=jit backend=sql "
    s *= "transport=$(U === nothing ? "epoll" : "uring") ckpt=$(ckpt ? "thread" : "auto") "
    s *= "hold_pages=$(ckpt ? HOLD_PAGES : 0) writer=$(writer ? 1 : 0) "
    s *= "group=$(U !== nothing && GROUP && !writer ? 1 : 0) feed=$(FEED_RANGE ? "range" : "plain") "
    s *= "debug=$(DEBUG ? 1 : 0) listening on $host:$port"
    logln(s)
    U === nothing ? serve(lfd) : serve_uring(U)
end

end # module
