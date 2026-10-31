// unit_server.c — translation unit 3: everything except SQLite, in one unit so the compiler can inline across the
// loop, the parsers, JWT and the app. Each component keeps its own C header, so any of them could be swapped for
// another object with the same ABI.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define BP_HTTP_H "http_gen.h"  // the parser generated from the API spec (build/http_gen.h)
#include "loop.c"
#include "../third_party/cursed-jwt/src/jwt.c"
#if (defined(APP_SLOW) && APP_SLOW == 1) || defined(APP_PICO) || defined(APP_CHECK_SLOW)
#include "picohttpparser.c"
#endif
#include "app.c"
#include "main.c"
#include "train.c"
