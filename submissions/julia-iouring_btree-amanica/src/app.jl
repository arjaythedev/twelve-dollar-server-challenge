# Entry point for the ahead-of-time build (build.sh: juliac's build script with --trim). The same server
# as src/main.jl, compiled to a native executable: no JIT at startup. Constants in src/ that read ENV
# (the dev-only JR_* switches) take their values when it is built.
include(joinpath(@__DIR__, "JuliaRaw.jl"))

function @main(args::Vector{String})::Cint
    JuliaRaw.main()
    return 0
end
