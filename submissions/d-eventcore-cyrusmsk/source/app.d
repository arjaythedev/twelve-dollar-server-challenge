module app;

import server;

version (PGO)
{
    extern (C) void __llvm_profile_initialize_file();
    extern (C) int __llvm_profile_write_file();
}

int main(string[] args)
{
    if (args.length > 1 && args[1] == "--pgo-train")
    {
        int rc = server.pgoTrain();
        version (PGO)
        {
            __llvm_profile_initialize_file();
            __llvm_profile_write_file();
        }
        return rc;
    }
    return server.run();
}
