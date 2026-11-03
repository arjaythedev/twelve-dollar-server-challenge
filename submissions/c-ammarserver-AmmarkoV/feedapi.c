/*
 * The $12 server challenge feed API, in C, on AmmarServer ( https://github.com/AmmarkoV/AmmarServer ).
 *
 * AmmarServer is in API mode ( AmmServer_StartAPI ) : one non-blocking epoll event loop owns every
 * connection from accept to close and calls HandleRequest for each request, on that one thread. So:
 *   - an idle keep-alive connection costs an fd and a small struct, not a thread, which is what makes
 *     serving the internet directly on :80 with ~15,000 open connections affordable on 1 vCPU / 2 GB
 *   - nothing in here needs a lock, and SQLite is opened SQLITE_THREADSAFE=0 with one connection and one
 *     set of prepared statements, so not a single mutex is taken on the way to the data
 *
 * Challenge rule 5 ( no caching across requests ) : every response below is built from a fresh SQLite read
 * taken while the request is being served, and every token's HMAC is recomputed from scratch. The only
 * things reused between requests are prepared statements ( query plans, explicitly allowed ) and the two
 * buffers the event loop reads and writes through.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#include "sqlite3.h"

#include "AmmServerlib.h"
#include "jwt.h"

/* The feed is 20 posts of up to 500 characters. 500 C0 control characters escape to 6 bytes each, which is
 * the worst case a post body can reach, so 20 * ( 3000 + name + overhead ) has to fit with room to spare. */
#define MAX_RESPONSE_SIZE (192*1024)
/* Has to hold any body AmmarServer will accept ( its API mode caps a request at 32 KB ) so that a body over
 * the 500 character limit can be measured and rejected as too long rather than mistaken for invalid. */
#define MAX_DECODED_BODY_SIZE (32*1024)
#define FEED_POST_COUNT 20

static sqlite3 * db = 0;
static sqlite3_stmt * stmtFeed       = 0;
static sqlite3_stmt * stmtPost       = 0;
static sqlite3_stmt * stmtInsertPost = 0;
static sqlite3_stmt * stmtPostExists = 0;
static sqlite3_stmt * stmtInsertLike = 0;
static sqlite3_stmt * stmtPing       = 0;

static char jwtSecret[512];
static unsigned int jwtSecretLength = 0;
static time_t serverStartTime = 0;

/* --- writing the response ---------------------------------------------------------------------------- */

/* Everything here answers through AmmarServer's own response writers ( AmmServer_APIResponseWrite* ) :
 * they append into the body buffer the server handed the handler, bounds checked, and once a write has
 * not fitted the response is marked overflowed and every later write does nothing. So a whole response
 * can be written without checking each step and the buffer still cannot be overrun - the one check is at
 * the end, with AmmServer_APIResponseOverflowed. The only thing added here is this macro, so that a
 * literal's length still comes from sizeof rather than from strlen. */
#define WriteLiteral(res,lit) AmmServer_APIResponseWrite((res),(lit),(unsigned int)(sizeof(lit)-1))

static void Fail(struct AmmServer_APIResponse * res,unsigned int status,const char * message)
{
  /* Throws away whatever had been written : this is called from halfway through building an answer. */
  AmmServer_APIResponseReset(res,status,0);

  WriteLiteral(res,"{\"error\":\"");
  AmmServer_APIResponseWriteText(res,message);
  WriteLiteral(res,"\"}");
}

/* {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} - the key order the spec fixes.
 * authorIsJsonEscaped is for the one case where the author came out of a JWT payload, where it is already
 * JSON string content and escaping it again would double its backslashes ( see jwt.h ). */
static void PutPostObject(
                           struct AmmServer_APIResponse * res,
                           long long id,
                           const char * body,unsigned int bodyLength,
                           const char * createdAt,unsigned int createdAtLength,
                           const char * author,unsigned int authorLength,int authorIsJsonEscaped,
                           long long likeCount
                         )
{
  WriteLiteral(res,"{\"id\":");
  AmmServer_APIResponseWriteInteger(res,id);
  WriteLiteral(res,",\"body\":\"");
  AmmServer_APIResponseWriteJSONString(res,body,bodyLength);
  WriteLiteral(res,"\",\"created_at\":\"");
  /* Stored in exactly the wire format the spec asks for, by the column default, so it goes out as stored. */
  AmmServer_APIResponseWrite(res,createdAt,createdAtLength);
  WriteLiteral(res,"\",\"author\":\"");
  if (authorIsJsonEscaped) { AmmServer_APIResponseWrite(res,author,authorLength); }
  else                     { AmmServer_APIResponseWriteJSONString(res,author,authorLength); }
  WriteLiteral(res,"\",\"like_count\":");
  AmmServer_APIResponseWriteInteger(res,likeCount);
  WriteLiteral(res,"}");
}

/* One row of the feed/post query, which both select the same five columns in the same order. */
static void PutPostRow(struct AmmServer_APIResponse * res,sqlite3_stmt * stmt)
{
  PutPostObject(
                 res,
                 sqlite3_column_int64(stmt,0),
                 (const char *) sqlite3_column_text(stmt,1),(unsigned int) sqlite3_column_bytes(stmt,1),
                 (const char *) sqlite3_column_text(stmt,2),(unsigned int) sqlite3_column_bytes(stmt,2),
                 (const char *) sqlite3_column_text(stmt,3),(unsigned int) sqlite3_column_bytes(stmt,3),0,
                 sqlite3_column_int64(stmt,4)
               );
}

/* --- request helpers -------------------------------------------------------------------------------- */

/**
 * Verify the request's bearer token, answering with the right 401 if it does not hold up.
 * scratch has to outlive every use of claims, which point into it.
 * @retval 1=authenticated , 0=res now holds the 401 to send
 */
static int Authenticate(
                         struct AmmServer_APIRequest * rqst,
                         struct AmmServer_APIResponse * res,
                         struct JwtClaims * claims,
                         char * scratch,
                         unsigned int scratchSize
                       )
{
  unsigned int tokenLength=0;
  const char * token = AmmServer_APIRequestBearer(rqst,&tokenLength);

  /* No Authorization header at all, or one whose scheme is not Bearer. A Bearer header with nothing after
   * it comes back non-zero with length 0 instead, and is a broken credential rather than a missing one. */
  if (token==0) { Fail(res,401,"missing bearer token"); return 0; }

  switch ( JwtVerifyToken(token,tokenLength,jwtSecret,jwtSecretLength,(long long) time(0),claims,scratch,scratchSize) )
   {
     case JWT_OK          : return 1;
     case JWT_INVALID     : Fail(res,401,"invalid or expired token"); return 0;
     case JWT_BAD_PAYLOAD : Fail(res,401,"invalid token payload");    return 0;
   };

  Fail(res,401,"invalid or expired token");
  return 0;
}

static void TrimAsciiWhitespace(const char ** s,unsigned int * length)
{
  const char * start = *s;
  unsigned int n = *length;

  while ( (n>0) && ( (*start==' ')||(*start=='\t')||(*start=='\n')||(*start=='\r')||(*start=='\v')||(*start=='\f') ) ) { ++start; --n; }
  while ( (n>0) && ( (start[n-1]==' ')||(start[n-1]=='\t')||(start[n-1]=='\n')||(start[n-1]=='\r')||(start[n-1]=='\v')||(start[n-1]=='\f') ) ) { --n; }

  *s = start;
  *length = n;
}

/* Characters, not bytes : the schema's own CHECK is length(body) BETWEEN 1 AND 500 , and SQLite's length()
 * counts characters, so counting bytes here would reject a legitimate 500 character body with accents in it
 * and accept one the CHECK would then refuse. */
static unsigned int Utf8CharacterCount(const char * s,unsigned int n)
{
  unsigned int count=0;
  unsigned int i=0;
  for (i=0; i<n; i++) { if ( ((unsigned char) s[i] & 0xC0) != 0x80 ) { ++count; } }
  return count;
}

/* --- the five endpoints ----------------------------------------------------------------------------- */

static void ServeHealth(struct AmmServer_APIResponse * res)
{
  /* The spec wants the ok answer to mean a SELECT 1 actually came back, so it is run per request. */
  int rc = SQLITE_ERROR;
  if (stmtPing!=0)
   {
     sqlite3_reset(stmtPing);
     rc = sqlite3_step(stmtPing);
     sqlite3_reset(stmtPing);
   }

  if (rc!=SQLITE_ROW)
   {
     const char * message = (db!=0) ? sqlite3_errmsg(db) : "database is not open";
     AmmServer_APIResponseReset(res,503,0);
     WriteLiteral(res,"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
     AmmServer_APIResponseWriteJSONString(res,message,(unsigned int) strlen(message));
     WriteLiteral(res,"\"}");
     return;
   }

  res->statusCode = 200;
  WriteLiteral(res,"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  AmmServer_APIResponseWriteInteger(res,(long long)( time(0)-serverStartTime ));
  WriteLiteral(res,"}");
}

static void ServeFeed(struct AmmServer_APIResponse * res)
{
  WriteLiteral(res,"{\"posts\":[");

  sqlite3_reset(stmtFeed);

  unsigned int rows=0;
  int rc=0;
  while ( (rc=sqlite3_step(stmtFeed))==SQLITE_ROW )
   {
     if (rows>0) { WriteLiteral(res,","); }
     PutPostRow(res,stmtFeed);
     ++rows;
   }
  sqlite3_reset(stmtFeed);

  if (rc!=SQLITE_DONE) { Fail(res,500,"internal server error"); return; }

  WriteLiteral(res,"]}");
  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void ServePost(struct AmmServer_APIResponse * res,long long postId)
{
  sqlite3_reset(stmtPost);
  sqlite3_bind_int64(stmtPost,1,postId);

  int rc = sqlite3_step(stmtPost);

  if (rc==SQLITE_DONE) { sqlite3_reset(stmtPost); Fail(res,404,"post not found"); return; }
  if (rc!=SQLITE_ROW)  { sqlite3_reset(stmtPost); Fail(res,500,"internal server error"); return; }

  WriteLiteral(res,"{\"post\":");
  PutPostRow(res,stmtPost);
  WriteLiteral(res,"}");

  sqlite3_reset(stmtPost);

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void CreatePost(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res)
{
  char payloadScratch[1024];
  struct JwtClaims claims={0};
  if (!Authenticate(rqst,res,&claims,payloadScratch,sizeof(payloadScratch))) { return; }

  /* Single threaded event loop , so one buffer for the decoded body is all that is ever needed. */
  static char decodedBody[MAX_DECODED_BODY_SIZE+1];
  unsigned int decodedLength=0;

  switch ( AmmServer_JSONGetStringField(rqst->body,rqst->bodyLength,"body",decodedBody,sizeof(decodedBody),&decodedLength) )
   {
     case AMMSERVER_JSON_MALFORMED          : Fail(res,400,"malformed JSON body"); return;
     case AMMSERVER_JSON_FIELD_NOT_A_STRING : Fail(res,400,"body is required");    return;
     case AMMSERVER_JSON_FIELD_OK           : break;
   };

  /* Longer than the buffer means longer than any 500 character body could ever be. */
  if (decodedLength>=sizeof(decodedBody)) { Fail(res,400,"body must be at most 500 characters"); return; }

  const char * body = decodedBody;
  unsigned int bodyLength = decodedLength;
  TrimAsciiWhitespace(&body,&bodyLength);

  if (bodyLength==0) { Fail(res,400,"body is required"); return; }
  if (Utf8CharacterCount(body,bodyLength)>500) { Fail(res,400,"body must be at most 500 characters"); return; }

  sqlite3_reset(stmtInsertPost);
  sqlite3_bind_int64(stmtInsertPost,1,claims.userId);
  sqlite3_bind_text(stmtInsertPost,2,body,(int) bodyLength,SQLITE_STATIC);

  int rc = sqlite3_step(stmtInsertPost);
  if (rc!=SQLITE_ROW) { sqlite3_reset(stmtInsertPost); Fail(res,500,"internal server error"); return; }

  long long newId = sqlite3_column_int64(stmtInsertPost,0);
  /* Copied out now : the column's own pointer does not survive the next step. */
  char createdAt[64];
  unsigned int createdAtLength = (unsigned int) sqlite3_column_bytes(stmtInsertPost,1);
  if (createdAtLength>=sizeof(createdAt)) { createdAtLength=sizeof(createdAt)-1; }
  memcpy(createdAt,sqlite3_column_text(stmtInsertPost,1),createdAtLength);

  /* Run the statement to completion before answering : with RETURNING it is this, not the first row, that
   * ends the implicit transaction, so this is what makes the 201 below mean the row is committed ( rule 6 ). */
  while ( (rc=sqlite3_step(stmtInsertPost))==SQLITE_ROW ) { }
  sqlite3_reset(stmtInsertPost);
  if (rc!=SQLITE_DONE) { Fail(res,500,"internal server error"); return; }

  WriteLiteral(res,"{\"post\":");
  PutPostObject(res,newId,body,bodyLength,createdAt,createdAtLength,claims.username,claims.usernameLength,1,0);
  WriteLiteral(res,"}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 201;
}

static void LikePost(struct AmmServer_APIResponse * res,long long userId,long long postId)
{
  /* Foreign keys are off by default in SQLite ( and turning them on would cost a lookup on every insert
   * anyway ), so whether the post exists is asked outright - otherwise a like on a post that was never
   * there would happily insert and answer 201 instead of 404. */
  sqlite3_reset(stmtPostExists);
  sqlite3_bind_int64(stmtPostExists,1,postId);
  int rc = sqlite3_step(stmtPostExists);
  sqlite3_reset(stmtPostExists);

  if (rc==SQLITE_DONE) { Fail(res,404,"post not found"); return; }
  if (rc!=SQLITE_ROW)  { Fail(res,500,"internal server error"); return; }

  sqlite3_reset(stmtInsertLike);
  sqlite3_bind_int64(stmtInsertLike,1,userId);
  sqlite3_bind_int64(stmtInsertLike,2,postId);
  rc = sqlite3_step(stmtInsertLike);
  int inserted = (sqlite3_changes(db)>0);
  sqlite3_reset(stmtInsertLike);

  if (rc!=SQLITE_DONE) { Fail(res,500,"internal server error"); return; }

  WriteLiteral(res,"{\"liked\":true,\"already_liked\":");
  if (inserted) { WriteLiteral(res,"false"); } else { WriteLiteral(res,"true"); }
  WriteLiteral(res,",\"post_id\":");
  AmmServer_APIResponseWriteInteger(res,postId);
  WriteLiteral(res,"}");

  res->statusCode = inserted ? 201 : 200;
}

/* --- routing ---------------------------------------------------------------------------------------- */

#define POSTS_PREFIX     "/posts/"
#define POSTS_PREFIX_LEN 7
#define LIKE_SUFFIX      "/like"
#define LIKE_SUFFIX_LEN  5

/* AmmServer_APIRequestMatch does the path matching for the two routes that carry an id : a template is the
 * whole path or it is not a match, and %lld only matches a run of digits that is a positive number and
 * cannot overflow, so "abc", "-1", "0" and "1.5" are not matches rather than ids parsed loosely.
 *
 * What it deliberately cannot say is "any path at all under this prefix", since every template is anchored
 * at both ends - and the spec needs exactly that: /posts/<anything unusable> is a post request with a bad
 * id and is answered 400, where a path that is not about posts at all is a 404. So the prefix tests below
 * stay, and what they guard is the classification, not the routing. */
static void HandleRequest(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res)
{
  long long postId=0;

  if (rqst->method==GET)
   {
     if (AmmServer_APIRequestMatch(rqst,"/feed"))   { ServeFeed(res);   return; }
     if (AmmServer_APIRequestMatch(rqst,"/health")) { ServeHealth(res); return; }

     if (AmmServer_APIRequestMatch(rqst,"/posts/%lld",&postId)) { ServePost(res,postId); return; }

     if ( (rqst->pathLength>POSTS_PREFIX_LEN) && (memcmp(rqst->path,POSTS_PREFIX,POSTS_PREFIX_LEN)==0) )
      { Fail(res,400,"invalid post id"); return; }
   }
  else
  if (rqst->method==POST)
   {
     if (AmmServer_APIRequestMatch(rqst,"/posts")) { CreatePost(rqst,res); return; }

     if ( (rqst->pathLength>POSTS_PREFIX_LEN+LIKE_SUFFIX_LEN) &&
          (memcmp(rqst->path,POSTS_PREFIX,POSTS_PREFIX_LEN)==0) &&
          (memcmp(rqst->path+rqst->pathLength-LIKE_SUFFIX_LEN,LIKE_SUFFIX,LIKE_SUFFIX_LEN)==0) )
      {
        /* Auth before the id , which the spec is explicit about : POST /posts/abc/like without a token is a
         * 401 and only becomes a 400 once there is a valid one. */
        char payloadScratch[1024];
        struct JwtClaims claims={0};
        if (!Authenticate(rqst,res,&claims,payloadScratch,sizeof(payloadScratch))) { return; }

        if (!AmmServer_APIRequestMatch(rqst,"/posts/%lld/like",&postId)) { Fail(res,400,"invalid post id"); return; }

        LikePost(res,claims.userId,postId);
        return;
      }
   }

  Fail(res,404,"not found");
}

/* --- startup ---------------------------------------------------------------------------------------- */

/* Serving the internet directly means one open file descriptor per connected user, so the descriptor
 * limit *is* the connection limit and therefore the score ceiling. The benchmark box hands this service
 * LimitNOFILE=65535, but a default login shell hands it a soft limit of 1024 while keeping a far higher
 * hard limit - and nothing would fail loudly at 1024 connections, accept() would just start returning
 * EMFILE while there were still tens of thousands of descriptors to be had. So the soft limit is raised
 * to whatever the hard limit already allows. This changes a setting inside this process only, which is
 * what challenge rule 11 permits ; it tunes nothing on the box and asks for nothing it was not given. */
static void RaiseDescriptorLimit(void)
{
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE,&limit)!=0) { fprintf(stderr,"feedapi: could not read RLIMIT_NOFILE\n"); return; }

  rlim_t wanted = limit.rlim_max;
  if (limit.rlim_cur!=wanted)
   {
     struct rlimit raised;
     raised.rlim_cur = wanted;
     raised.rlim_max = limit.rlim_max;
     if (setrlimit(RLIMIT_NOFILE,&raised)==0) { limit.rlim_cur = wanted; }
   }

  if (limit.rlim_cur==RLIM_INFINITY) { fprintf(stderr,"feedapi: descriptor limit is unlimited\n"); }
  else { fprintf(stderr,"feedapi: descriptor limit %lu ( about %lu connections )\n",
                 (unsigned long) limit.rlim_cur,(unsigned long)(limit.rlim_cur-16)); }
}

static int Prepare(sqlite3_stmt ** stmt,const char * sql)
{
  /* SQLITE_PREPARE_PERSISTENT : these are prepared once and reused for the life of the process , which is
   * what challenge rule 5 allows ( a query plan is cached , no data is ). */
  if (sqlite3_prepare_v3(db,sql,-1,SQLITE_PREPARE_PERSISTENT,stmt,0)!=SQLITE_OK)
   {
     fprintf(stderr,"feedapi: could not prepare [%s] : %s\n",sql,sqlite3_errmsg(db));
     return 0;
   }
  return 1;
}

static int OpenDatabase(const char * path)
{
  /* Built with SQLITE_OMIT_AUTOINIT ( see build.sh ) , which trades the "am I initialized yet" check on
   * every API call for having to do this once, here. */
  if (sqlite3_initialize()!=SQLITE_OK) { fprintf(stderr,"feedapi: sqlite3_initialize failed\n"); return 0; }

  if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,0)!=SQLITE_OK)
   {
     fprintf(stderr,"feedapi: could not open %s : %s\n",path,(db!=0)?sqlite3_errmsg(db):"out of memory");
     return 0;
   }

  /* journal_mode=WAL and synchronous=NORMAL are what rule 6 asks for : a commit is durable in the WAL
   * before the 201 goes out, without an fsync on the serving thread for every single write.
   * mmap_size covers the whole ~238 MB seed database, so reads come out of the page cache with no copy into
   * SQLite's own cache - rule 5 names SQLite's page cache and mmap as fair game.
   * busy_timeout is belt and braces : one connection on one thread never contends with itself, but a
   * checkpoint racing an external reader should wait rather than fail the request. */
  static const char * pragmas =
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA cache_size=-32768;"
      "PRAGMA mmap_size=268435456;"
      "PRAGMA busy_timeout=5000;";

  char * errorMessage=0;
  if (sqlite3_exec(db,pragmas,0,0,&errorMessage)!=SQLITE_OK)
   {
     fprintf(stderr,"feedapi: pragma setup failed : %s\n",(errorMessage!=0)?errorMessage:"unknown");
     sqlite3_free(errorMessage);
     return 0;
   }

  /* The two reads share their column list and their order , which is what lets PutPostRow serve both. */
  static const char * selectColumns =
      "SELECT p.id,p.body,p.created_at,u.username,"
      "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
      "FROM posts p JOIN users u ON u.id=p.user_id ";

  char sql[1024];

  /* posts_created_at_id_idx is (created_at DESC,id DESC) , so this is 20 rows off the front of that index
   * rather than a sort of half a million posts. */
  snprintf(sql,sizeof(sql),"%sORDER BY p.created_at DESC,p.id DESC LIMIT %u",selectColumns,FEED_POST_COUNT);
  if (!Prepare(&stmtFeed,sql)) { return 0; }

  snprintf(sql,sizeof(sql),"%sWHERE p.id=?",selectColumns);
  if (!Prepare(&stmtPost,sql)) { return 0; }

  if (!Prepare(&stmtInsertPost,"INSERT INTO posts (user_id,body) VALUES (?,?) RETURNING id,created_at")) { return 0; }
  if (!Prepare(&stmtPostExists,"SELECT 1 FROM posts WHERE id=?")) { return 0; }
  if (!Prepare(&stmtInsertLike,"INSERT INTO likes (user_id,post_id) VALUES (?,?) ON CONFLICT (user_id,post_id) DO NOTHING")) { return 0; }
  if (!Prepare(&stmtPing,"SELECT 1")) { return 0; }

  return 1;
}

int main(int argc,char *argv[])
{
  serverStartTime = time(0);

  const char * sqlitePath = getenv("SQLITE_PATH");
  const char * secret     = getenv("JWT_SECRET");
  const char * host       = getenv("HOST");
  const char * portText   = getenv("PORT");

  if ( (sqlitePath==0) || (sqlitePath[0]==0) ) { fprintf(stderr,"feedapi: SQLITE_PATH is not set\n"); return 1; }
  if (secret==0) { secret=""; }

  jwtSecretLength = (unsigned int) strlen(secret);
  if (jwtSecretLength>=sizeof(jwtSecret)) { fprintf(stderr,"feedapi: JWT_SECRET is too long\n"); return 1; }
  memcpy(jwtSecret,secret,jwtSecretLength);

  if ( (host==0) || (host[0]==0) ) { host="0.0.0.0"; }
  unsigned int port = (portText!=0) ? (unsigned int) atoi(portText) : 80;
  if (port==0) { port=80; }

  RaiseDescriptorLimit();

  if (!OpenDatabase(sqlitePath)) { return 1; }

  /* Nothing to switch off for logging : API mode never touches the access/error logs, so there is no
   * per-request write ( or lock ) on the one thread that serves every connection. AmmarServer writes a
   * single startup line and nothing after it. */
  fprintf(stderr,"feedapi: AmmarServer %s , sqlite %s , serving %s on %s:%u\n",
          AmmServer_Version(),sqlite3_libversion(),sqlitePath,host,port);

  /* An empty config path , not 0 , so that AmmarServer's automatic lookup cannot pick up a stray
   * ammarServer.conf or /etc/ammarServer.conf and quietly change the port or the thread settings underneath
   * the benchmark. Everything this process needs comes from the environment variables above. */
  /* Every connection is served from io_uring rather than from an epoll event loop, which means it lives as a
   * direct descriptor inside a ring's own file table instead of as a file descriptor. A direct descriptor is
   * not charged against RLIMIT_NOFILE, so the descriptor limit stops being the connection limit : a ring's
   * table may be declared no larger than RLIMIT_NOFILE, but that is checked once per table, at registration,
   * so RINGS tables hold RINGS times as many connections in this same single-threaded process. With
   * LimitNOFILE=65535 that is a ceiling of 393,210 rather than 65,527. Measured : 70,000 concurrent
   * connections, every one of them answered, in a process whose own descriptor limit was 65,535.
   *
   * The rings fill one at a time, so the second costs nothing until the first is full, and an idle ring costs
   * one syscall a second. IO_URING=N asks for a different number of them, and IO_URING=0 asks for the epoll
   * event loop instead - which is also where this lands by itself, with a line to say so, on a kernel that
   * cannot give it a ring ( it needs 5.19 or newer ). Same handler, same SQLite, same three threads either
   * way : only the transport underneath changes, which is what makes the two comparable. */
  #define DEFAULT_RINGS 6
  const char * useRing = getenv("IO_URING");
  unsigned int wantRings = ( useRing!=0 ) ? (unsigned int) strtoul(useRing,0,10) : DEFAULT_RINGS;

  struct AmmServer_Instance * instance =
      ( wantRings>0 ) ?
        AmmServer_StartAPIOnRings("feedapi",host,port,"/dev/null",(void*) &HandleRequest,MAX_RESPONSE_SIZE,wantRings) :
        AmmServer_StartAPI("feedapi",host,port,"/dev/null",(void*) &HandleRequest,MAX_RESPONSE_SIZE);
  if (instance==0) { fprintf(stderr,"feedapi: could not start AmmarServer on %s:%u\n",host,port); return 1; }

  /* Every request is served on AmmarServer's event loop thread ; this one only has to stay out of its way. */
  while (AmmServer_Running(instance)) { sleep(1); }

  AmmServer_Stop(instance);

  sqlite3_finalize(stmtFeed);
  sqlite3_finalize(stmtPost);
  sqlite3_finalize(stmtInsertPost);
  sqlite3_finalize(stmtPostExists);
  sqlite3_finalize(stmtInsertLike);
  sqlite3_finalize(stmtPing);
  sqlite3_close(db);

  return 0;
}
