module JuliaRaw

include("buf.jl")
include("json.jl")
include("jwt.jl")
include("db.jl")
include("http.jl")
include("handlers.jl")
include("epoll.jl")

function main()
    START[] = time()
    DEBUG[] = get(ENV, "JR_DEBUG", "") == "1"     # dev only: periodic allocation/GC stats in the log
    path = get(ENV, "SQLITE_PATH", "")
    isempty(path) && error("SQLITE_PATH is not set")
    init_jwt(get(ENV, "JWT_SECRET", "twelve-dollar-challenge"))
    db_open(path)
    host = get(ENV, "HOST", "0.0.0.0")
    port = parse(Int, get(ENV, "PORT", "80"))
    lfd = listen_tcp(host, port)
    println("memory: total=$(Sys.total_memory() >> 20)MB free=$(Sys.free_memory() >> 20)MB")
    println("julia-raw-amanica julia=$(VERSION) backend=sql transport=epoll debug=$(DEBUG[] ? 1 : 0) listening on $host:$port")
    flush(stdout)
    serve(lfd)
end

end # module
