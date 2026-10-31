module app;

import server;
import core.stdc.string : strcmp;

version (PGO)
{
    extern (C) void __llvm_profile_initialize_file();
    extern (C) int __llvm_profile_write_file();
}

extern (C) int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--pgo-train") == 0)
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
