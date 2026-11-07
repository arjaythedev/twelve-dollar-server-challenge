# Dev tools for dev/ct.sh (run on the host, in the dev/ project environment: JSON.jl).
#
#   julia dev/tools.jl record <run dir> <generator exit code> <csv>
#       Turns one run's raw output (results/<run>/) into a row of stress-results.csv and a console line.
#       env: GEN DEBUG LABEL NOTES CPUS MEM VUS RAMP DUR COMMIT
#       Exit status 0 = the run passed the scoring thresholds (p95 < 500 ms, p99 < 1 s, errors < 1%).
#   julia dev/tools.jl report <csv> <README.md>
#       Rewrites the progress section of README.md (between the results markers): one line per change
#       (LABEL, in the order first measured). Labels starting with "probe" or "smoke" are skipped.

using Printf, Dates, JSON

const COLUMNS = ["timestamp", "commit", "label", "gen", "debug", "julia", "backend", "transport", "cpus", "mem",
    "vus", "ramp", "duration", "requests", "req_s", "est_users", "p95_ms", "p99_ms", "max_ms",
    "err_pct", "pass", "failed_on", "srv_cpu_avg", "srv_cpu_max", "srv_rss_peak_mb",
    "srv_cgmem_peak_mb", "srv_conns_peak", "alloc_b_per_req", "gc_ms", "gc_max_pause_ms", "gen_mem_peak",
    "boot_ms", "notes"]
const RENAMED = Dict("k6_mem_peak" => "gen_mem_peak")
const USERS_PER_REQ_S = 10.6     # leaderboard: 310k users <-> 29.3k req/s
const BASE_FDS = 23              # fds julia holds without connections (measured: 60,000 users -> 60,023 fds)

# ---------------------------------------------------------------- small helpers

readall(path) = isfile(path) ? read(path, String) : ""
fmt(x, d = 3) = x === nothing ? "" : Printf.format(Printf.Format("%.$(d)f"), x)
kv(line) = Dict(m[1] => m[2] for m in eachmatch(r"(\w+)=(\S+)", line))
median(xs) = (s = sort(xs); n = length(s); isodd(n) ? s[(n+1)÷2] : (s[n÷2] + s[n÷2+1]) / 2)
function commas(x::Real)                      # 12345.6 -> "12,346"
    s = string(round(Int, x))
    neg = startswith(s, "-"); neg && (s = s[2:end])
    parts = [s[max(1, i - 2):i] for i in length(s):-3:1]
    (neg ? "-" : "") * join(reverse(parts), ",")
end

# Our CSV never needs quoting: dev/ct.sh replaces commas in free text with ';'.
function read_csv(path)
    isfile(path) || return Dict{String,String}[]
    lines = filter(!isempty, readlines(path))
    isempty(lines) && return Dict{String,String}[]
    hdr = [get(RENAMED, h, h) for h in split(lines[1], ',')]
    [Dict{String,String}(h => String(v) for (h, v) in zip(hdr, split(l, ','; limit = length(hdr)))) for l in lines[2:end]]
end
function write_csv(path, rows)
    open(path, "w") do io
        println(io, join(COLUMNS, ','))
        for r in rows
            println(io, join((replace(get(r, c, ""), ',' => ';') for c in COLUMNS), ','))
        end
    end
end

# ---------------------------------------------------------------- record

function record(out, rc, csv_path)
    E(k, d = "") = get(ENV, k, d)
    gen = E("GEN", "k6")
    row = Dict{String,String}(c => "" for c in COLUMNS)
    merge!(row, Dict("timestamp" => Dates.format(now(UTC), "yyyy-mm-ddTHH:MM:SSZ"), "commit" => E("COMMIT"),
        "label" => E("LABEL"), "gen" => gen, "debug" => E("DEBUG", "0"), "cpus" => E("CPUS"), "mem" => E("MEM"),
        "vus" => E("VUS"), "ramp" => gen == "blast" ? "" : E("RAMP"), "duration" => E("DUR"), "notes" => E("NOTES")))
    ok = false
    failed = String[]

    if gen == "k6"
        m = try
            JSON.parse(readall(joinpath(out, "summary.json")))["metrics"]
        catch
            nothing
        end
        if m !== nothing
            dur, fail, reqs = get(m, "http_req_duration", Dict()), get(m, "http_req_failed", Dict()), get(m, "http_reqs", Dict())
            row["requests"] = haskey(reqs, "count") ? string(round(Int, reqs["count"])) : ""
            row["req_s"] = fmt(get(reqs, "rate", nothing), 1)
            row["p95_ms"] = fmt(get(dur, "p(95)", nothing)); row["p99_ms"] = fmt(get(dur, "p(99)", nothing))
            row["max_ms"] = fmt(get(dur, "max", nothing), 1)
            row["err_pct"] = haskey(fail, "value") ? fmt(100 * fail["value"]) : ""
            for k in ("http_req_duration", "http_req_failed"), (n, crossed) in get(get(m, k, Dict()), "thresholds", Dict())
                crossed === true && push!(failed, n)
            end
            ok = rc == 0
        else
            push!(failed, "k6 exit $rc")
        end
    else                                      # blast / users: one or more wrk shards
        files = sort(filter(f -> occursin(r"^shard-\d+\.txt$", f), readdir(out)), by = f -> parse(Int, match(r"\d+", f).match))
        files = isempty(files) ? [joinpath(out, "wrk.txt")] : joinpath.(out, files)
        shards = Tuple{Any,Any}[]
        for fn in files
            t = readall(fn)
            js = collect(eachmatch(r"BLAST_JSON (\{.*\})", t)); qs = collect(eachmatch(r"BLAST_Q (\[.*\])", t))
            isempty(js) || push!(shards, (JSON.parse(js[end][1]), isempty(qs) ? nothing : JSON.parse(qs[end][1])))
        end
        if !isempty(shards)
            errkeys = ("err_status", "err_connect", "err_read", "err_write", "err_timeout")
            reqs = sum(b["requests"] for (b, _) in shards)
            errs = sum(b[k] for (b, _) in shards for k in errkeys)
            rps = sum(b["requests"] / (b["duration_us"] / 1e6) for (b, _) in shards if b["duration_us"] > 0; init = 0.0)
            function pct(p)               # merged quantile: each shard's 200 quantiles weighted by its request count
                samples = sort([(Float64(v), b["requests"] / 200) for (b, q) in shards if q !== nothing for v in q])
                isempty(samples) && return maximum(b[p == 95 ? "p95_us" : "p99_us"] for (b, _) in shards)
                target, acc = sum(last.(samples)) * p / 100, 0.0
                for (v, w) in samples
                    acc += w
                    acc >= target && return v
                end
                samples[end][1]
            end
            p95, p99 = pct(95) / 1000, pct(99) / 1000
            err = 100 * errs / max(reqs + sum(b["err_connect"] for (b, _) in shards), 1)
            row["requests"] = string(round(Int, reqs)); row["req_s"] = fmt(rps, 1)
            row["p95_ms"] = fmt(p95); row["p99_ms"] = fmt(p99)
            row["max_ms"] = fmt(maximum(b["max_us"] for (b, _) in shards) / 1000, 1); row["err_pct"] = fmt(err)
            row["est_users"] = gen == "blast" ? string(round(Int, rps * USERS_PER_REQ_S)) : ""
            p95 >= 500 && push!(failed, "p(95)<500")
            p99 >= 1000 && push!(failed, "p(99)<1000")
            err >= 1 && push!(failed, "errors<1%")
            length(shards) < length(files) && push!(failed, "$(length(files) - length(shards)) shard(s) without results")
            ok = isempty(failed) && reqs > 0
        else
            push!(failed, "wrk exit $rc")
        end
    end
    row["pass"] = ok ? "1" : "0"
    row["failed_on"] = join(failed, " ")

    # host-side samples (dev/ct.sh sampler)
    cpu, cg, rss, gm, fds = Int[], Int[], Int[], Float64[], Int[]
    for line in eachline(IOBuffer(readall(joinpath(out, "stats.log"))))
        d = kv(line)
        if haskey(d, "cpu")
            push!(cpu, parse(Int, d["cpu"])); push!(cg, parse(Int, d["cgmem_mb"])); push!(rss, parse(Int, d["rss_mb"]))
            haskey(d, "fds") && push!(fds, parse(Int, d["fds"]))
        end
        mm = match(r"^([\d.]+)([kMG]?B)", get(d, "genmem", ""))
        mm === nothing || push!(gm, parse(Float64, mm[1]) * Dict("B" => 1e-6, "kB" => 1e-3, "MB" => 1.0, "GB" => 1e3)[mm[2]])
    end
    busy = filter(>(2), cpu); isempty(busy) && (busy = cpu)
    log = readall(joinpath(out, "server.log"))
    info = occursin("listening", log) ? kv(last(first(split(log, "listening")), 300)) : Dict{String,String}()
    row["julia"] = get(info, "julia", ""); row["backend"] = get(info, "backend", ""); row["transport"] = get(info, "transport", "")
    isempty(busy) || (row["srv_cpu_avg"] = string(round(Int, sum(busy) / length(busy))))
    isempty(cpu) || (row["srv_cpu_max"] = string(maximum(cpu)))
    isempty(rss) || (row["srv_rss_peak_mb"] = string(maximum(rss)))
    isempty(cg) || (row["srv_cgmem_peak_mb"] = string(maximum(cg)))
    isempty(gm) || (row["gen_mem_peak"] = @sprintf("%.0fMB", maximum(gm)))
    bm = match(r"boot_ms=(\d+)", log); bm === nothing || (row["boot_ms"] = bm[1])

    # allocation / GC from the server's "stats" lines (--debug); only windows with real load count
    st = [kv(l) for l in split(log, '\n') if startswith(l, "stats ")]
    loaded = filter(x -> parse(Int, get(x, "reqs", "0")) >= 1000, st)
    if !isempty(loaded)
        per = sort([parse(Int, x["alloc_per_req"]) for x in loaded])
        row["alloc_b_per_req"] = string(per[length(per)÷2+1])
        row["gc_ms"] = string(sum(parse(Int, x["gc_ms"]) for x in st))
        row["gc_max_pause_ms"] = string(maximum(parse(Int, x["max_pause_ms"]) for x in st))
    end

    # open connections ~ julia's fds minus the ones it holds anyway (--debug)
    conns = isempty(fds) ? nothing : maximum(fds) - BASE_FDS
    conns === nothing || (row["srv_conns_peak"] = string(max(conns, 0)))
    nv = tryparse(Int, row["vus"])
    if gen == "users" && conns !== nothing && nv !== nothing && nv > 0 && conns < 0.99 * nv
        ok = false; row["pass"] = "0"
        row["failed_on"] = strip(row["failed_on"] * " only $(max(conns, 0)) of $nv users connected")
    end

    rows = read_csv(csv_path)
    for r in rows
        isempty(get(r, "gen", "")) && (r["gen"] = "k6")
        isempty(get(r, "debug", "")) && (r["debug"] = "1")   # runs before the flag existed sampled everything
    end
    push!(rows, row)
    write_csv(csv_path, rows)

    what = gen == "blast" ? "connections" : "users"
    println("   ", ok ? "PASS" : "FAIL", "  $gen $(row["vus"]) $what  boot=$(row["boot_ms"])ms req/s=$(row["req_s"])",
        isempty(row["est_users"]) ? "" : " (~$(row["est_users"]) users)",
        " p95=$(row["p95_ms"])ms p99=$(row["p99_ms"])ms err=$(row["err_pct"])%",
        " cpu avg/max=$(row["srv_cpu_avg"])/$(row["srv_cpu_max"])% rss=$(row["srv_rss_peak_mb"])MB",
        " conns=$(row["srv_conns_peak"]) alloc/req=$(row["alloc_b_per_req"])B gc=$(row["gc_ms"])ms",
        " $gen=$(row["gen_mem_peak"]) $(row["failed_on"])")
    gen == "users" && conns === nothing &&
        println("   note: connections not counted (run with --debug to check that every user connected)")
    !ok && !isempty(cpu) && maximum(cpu) < 85 &&
        println("   note: server CPU never above 85%: the limit may be the load generator, the host or a connection limit, not the server")
    ok ? 0 : 1
end

# ---------------------------------------------------------------- report

function report(csv_path, readme_path)
    rows = read_csv(csv_path)
    steps = String[]
    best = Dict{String,Dict{String,Any}}()
    blast_runs = Dict{String,Vector{Float64}}()
    for r in rows
        lab = r["label"]
        any(p -> startswith(lowercase(lab), p), ("probe", "smoke")) && continue   # measurements, not changes
        haskey(best, lab) || (push!(steps, lab); best[lab] = Dict{String,Any}("k6" => nothing, "users" => nothing, "blast" => nothing))
        r["pass"] == "1" || continue
        gen = isempty(get(r, "gen", "")) ? "k6" : r["gen"]
        if gen == "blast"                     # blast varies ~10% run to run: keep all, take the median
            push!(get!(blast_runs, lab, Float64[]), parse(Float64, r["req_s"]))
            continue
        end
        v = parse(Int, r["vus"])
        b = best[lab]
        if b[gen] === nothing || v > b[gen] || (v == b[gen] && gen == "users")
            b[gen] = v
            gen == "users" && (b["rss"] = get(r, "srv_rss_peak_mb", ""))
        end
    end
    for (lab, xs) in blast_runs
        best[lab]["blast"] = median(xs); best[lab]["nblast"] = length(xs)
    end
    delta(cur, prev) = cur === nothing || prev === nothing ? "" : @sprintf(" (%+.0f%%)", 100 * (cur - prev) / prev)

    L = ["Best local result per change. Raw runs: [stress-results.csv](stress-results.csv); how to run: above.",
        "Blast req/s (max throughput at 1 CPU): median of the passing runs (they vary ~10%), with % change against the",
        "previous change and the number of runs. Changes under ~10% are noise unless they repeat.",
        "k6 / wrk users = highest passing run (not always a full ladder). k6 is capped near 20k users on this host;",
        "wrk users (`GEN=users`, think time per connection) are cheap, so they reach the connection limits.", "",
        "| change | k6 users | wrk users (server RSS) | blast req/s |", "|---|--:|--:|--:|"]
    prev = Dict{String,Any}("k6" => nothing, "users" => nothing, "blast" => nothing)
    for lab in steps
        b = best[lab]
        k6 = b["k6"] === nothing ? "–" : commas(b["k6"])
        us = b["users"] === nothing ? "–" : "$(commas(b["users"])) ($(isempty(get(b, "rss", "")) ? "?" : b["rss"]) MB)"
        bl = b["blast"] === nothing ? "–" : "$(commas(b["blast"]))$(delta(b["blast"], prev["blast"])), n=$(get(b, "nblast", 1))"
        push!(L, "| $lab | $k6 | $us | $bl |")
        for g in ("k6", "users", "blast")
            b[g] === nothing || (prev[g] = b[g])
        end
    end

    readme = read(readme_path, String)
    bm, em = "<!-- results:begin -->", "<!-- results:end -->"
    block = bm * "\n" * join(L, "\n") * "\n" * em
    if occursin(bm, readme)
        readme = first(split(readme, bm)) * block * last(split(readme, em))
    else
        readme = rstrip(readme) * "\n\n## Progress\n\n" * block * "\n"
    end
    write(readme_path, readme)
    println("README.md updated: $(length(steps)) changes from $(length(rows)) runs")
    0
end

# ---------------------------------------------------------------- main

if abspath(PROGRAM_FILE) == @__FILE__
    cmd = isempty(ARGS) ? "" : ARGS[1]
    code = if cmd == "record" && length(ARGS) == 4
        record(ARGS[2], parse(Int, ARGS[3]), ARGS[4])
    elseif cmd == "report" && length(ARGS) == 3
        report(ARGS[2], ARGS[3])
    else
        println(stderr, "usage: julia dev/tools.jl record <run dir> <exit code> <csv> | report <csv> <README.md>")
        2
    end
    exit(code)
end
