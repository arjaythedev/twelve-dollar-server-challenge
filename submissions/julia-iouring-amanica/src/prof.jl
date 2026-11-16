# Dev-only sampling profile of the event loop (JR_PROFILE="<start s>,<seconds>", set by dev/ct.sh PROFILE=).
# Fixed when the module is loaded, like DEBUG: unset, `@prof` expands to nothing and Profile is never loaded.
# Samples every 5 ms from <start> seconds after startup for <seconds>, then prints a flat table sorted by
# self time (Julia and C frames: SQLite, libcrypto, the syscall wrappers) to stderr, i.e. the server log.
# Time blocked in io_uring_enter waiting for work counts as samples too: compare with loop_wait_pct.

const PROFILE = get(ENV, "JR_PROFILE", "")
isempty(PROFILE) || using Profile

macro prof(ex)
    isempty(PROFILE) ? nothing : esc(ex)
end

const PROF_AT = Ref(0.0)
const PROF_UNTIL = Ref(0.0)
const PROF_STATE = Ref(0)            # 0 waiting, 1 sampling, 2 done

function prof_init!()
    a, b = parse.(Float64, split(PROFILE, ','))
    PROF_AT[] = time() + a
    PROF_UNTIL[] = PROF_AT[] + b
    Profile.init(n = 10^7, delay = 0.005)   # 1 ms cost blast ~20% of its req/s
    println("profile: sampling from +$(a)s for $(b)s")
end

function prof_tick!()
    PROF_STATE[] == 2 && return
    t = time()
    if PROF_STATE[] == 0
        t >= PROF_AT[] || return
        PROF_STATE[] = 1
        Profile.start_timer()
    elseif t >= PROF_UNTIL[]
        Profile.stop_timer()
        PROF_STATE[] = 2
        # Profile's flat table (Count = inclusive samples, Overhead = self), re-sorted here by self samples.
        # Note: Profile's own listener task idles in pthread_cond_wait and adds one libc "[unknown function]"
        # sample per sample: that row's self count is mostly the listener, not the event loop.
        buf = IOBuffer()
        Profile.print(IOContext(buf, :displaysize => (10_000, 250)); format = :flat, C = true, mincount = 10)
        rows = [l for l in split(String(take!(buf)), '\n') if occursin(r"^\s*\d+\s+\d+\s", l)]
        self(l) = parse(Int, split(l)[2])
        sort!(rows; by = self, rev = true)
        println(stderr, "profile: begin (Count = inclusive samples, Overhead = self; sorted by self)")
        foreach(l -> println(stderr, l), rows[1:min(end, 80)])
        incl = sort(filter(l -> occursin("julia-iouring-amanica/src/", l), rows); by = l -> parse(Int, split(l)[1]), rev = true)
        println(stderr, "profile: our code by inclusive samples")
        foreach(l -> println(stderr, l), incl[1:min(end, 40)])
        println(stderr, "profile: end")
        flush(stderr)
    end
end
