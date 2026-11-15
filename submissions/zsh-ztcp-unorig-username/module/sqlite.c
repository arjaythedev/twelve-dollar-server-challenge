/*
 * zsh/sqlite: a SQLite binding for the Z shell, built into zsh as one of its own modules.
 *
 *   zsqlite -o FILE          open the database (one per shell)
 *   zsqlite SQL [ARG ...]    run SQL with the ARGs bound as text to ?1, ?2, ...
 *                            every column of every row goes into $reply (NULL is ""),
 *                            status 0; on an error, status 1 and the message in $REPLY
 *
 * Prepared statements are kept by SQL text and reused (a plan cache, not a data cache).
 */
#include "sqlite.mdh"
#include "sqlite.pro"
#include <sqlite3.h>

static sqlite3 *db;
struct stmt { struct stmt *next; char *sql; sqlite3_stmt *st; };
static struct stmt *stmts;

static int
fail(void)
{
    setsparam("REPLY", metafy((char *) (db ? sqlite3_errmsg(db) : "no database"), -1, META_DUP));
    return 1;
}

static sqlite3_stmt *
prepare(char *sql)
{
    struct stmt *s;
    sqlite3_stmt *st;

    for (s = stmts; s; s = s->next)
	if (!strcmp(s->sql, sql))
	    return s->st;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, NULL) != SQLITE_OK)
	return NULL;
    s = (struct stmt *) zalloc(sizeof *s);
    s->sql = ztrdup(sql);
    s->st = st;
    s->next = stmts;
    stmts = s;
    return st;
}

/**/
static int
bin_zsqlite(char *nam, char **args, Options ops, UNUSED(int func))
{
    sqlite3_stmt *st;
    LinkList rows;
    char *sql, **arr, **p;
    int i, n, len, rc;

    if (OPT_ISSET(ops, 'o')) {
	sql = unmetafy(dupstring(args[0]), NULL);
	if (db || sqlite3_open(sql, &db) != SQLITE_OK)
	    return fail();
	return 0;
    }
    if (!db)
	return fail();
    sql = unmetafy(dupstring(*args++), NULL);
    if (!(st = prepare(sql)))
	return fail();
    for (i = 1; *args; i++, args++) {
	char *a = dupstring(*args);
	unmetafy(a, &len);
	sqlite3_bind_text(st, i, a, len, SQLITE_TRANSIENT);
    }
    rows = newlinklist();
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
	n = sqlite3_column_count(st);
	for (i = 0; i < n; i++) {
	    const char *t = (const char *) sqlite3_column_text(st, i);
	    len = sqlite3_column_bytes(st, i);
	    addlinknode(rows, metafy((char *) (t ? t : ""), len, META_HEAPDUP));
	}
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE)
	return fail();
    arr = p = (char **) zalloc((countlinknodes(rows) + 1) * sizeof(char *));
    while (firstnode(rows))
	*p++ = ztrdup((char *) ugetnode(rows));
    *p = NULL;
    setaparam("reply", arr);
    return 0;
}

static struct builtin bintab[] = {
    BUILTIN("zsqlite", 0, bin_zsqlite, 1, -1, 0, "o", NULL),
};

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL, 0,
    NULL, 0,
    NULL, 0,
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(UNUSED(Module m))
{
    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    return 0;
}
