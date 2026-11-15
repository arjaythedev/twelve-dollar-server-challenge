import Feed.Emit

/-- `lake exe derive <dir>`: write the generated application layer to `<dir>/app_gen.c`. -/
def main (args : List String) : IO UInt32 := do
  let dir := args.headD "gen"
  IO.FS.createDirAll dir
  IO.FS.writeFile (dir ++ "/app_gen.c") Feed.Emit.appGen
  IO.FS.writeFile (dir ++ "/transport_gen.h") Feed.Emit.transportGen
  IO.println s!"wrote {dir}/app_gen.c and {dir}/transport_gen.h"
  return 0
