// Test-only replacement for the application's sqlite3_exec calls. Never linked by build.sh.
#include "sqlite3.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>

extern "C" int test_exec(sqlite3 *db, const char *sql,
                        int (*callback)(void *, int, char **, char **), void *arg, char **error) {
  if (!strcmp(sql, "COMMIT")) {
    std::string directory = std::getenv("COMMIT_TEST_DIR");
    std::string hold = directory + "/hold", entered = directory + "/entered", fail = directory + "/fail";
    if (!access(hold.c_str(), F_OK)) {
      int fd = open(entered.c_str(), O_CREAT | O_WRONLY, 0600);
      if (fd >= 0) close(fd);
      for (int i = 0; i < 500 && !access(hold.c_str(), F_OK); ++i) usleep(10000);
    }
    if (!unlink(fail.c_str())) return SQLITE_FULL;
  }
  return sqlite3_exec(db, sql, callback, arg, error);
}
