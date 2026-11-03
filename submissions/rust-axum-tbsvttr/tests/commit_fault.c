/* Test-only fault injection at SQLite's commit hook; not part of the server. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
static int commit(void *unused) {
  (void)unused;
  const char *dir=getenv("COMMIT_TEST_DIR");
  if (!dir) return 0;
  char hold[4096],entered[4096],fail[4096];
  snprintf(hold,sizeof(hold),"%s/hold",dir);
  snprintf(entered,sizeof(entered),"%s/entered",dir);
  snprintf(fail,sizeof(fail),"%s/fail",dir);
  if (!access(hold,F_OK)) {
    int fd=open(entered,O_CREAT|O_WRONLY,0600);
    if(fd>=0) close(fd);
    for(int i=0;i<500 && !access(hold,F_OK);++i) usleep(10000);
  }
  return !unlink(fail);
}
extern int __real_sqlite3_open_v2(const char *,sqlite3 **,int,const char *);
int __wrap_sqlite3_open_v2(const char *path,sqlite3 **db,int flags,const char *vfs) {
  int rc=__real_sqlite3_open_v2(path,db,flags,vfs);
  if(rc==SQLITE_OK) sqlite3_commit_hook(*db,commit,NULL);
  return rc;
}
