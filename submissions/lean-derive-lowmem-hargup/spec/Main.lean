import Feed.Emit
import Feed.Frame
import Feed.Sqe

/-- `lake exe derive <dir>`: write the application and its job/framing/SQE headers. -/
def main (args : List String) : IO UInt32 := do
  let dir := args.headD "gen"
  IO.FS.createDirAll dir
  IO.FS.writeFile (dir ++ "/app_gen.c") Feed.Emit.appGen
  IO.FS.writeFile (dir ++ "/job_gen.h") Feed.Emit.jobGen
  IO.FS.writeFile (dir ++ "/frame_gen.h") Feed.Frame.header
  IO.FS.writeFile (dir ++ "/sqe_gen.h") Feed.Sqe.header
  IO.println s!"wrote {dir}/app_gen.c"
  return 0
