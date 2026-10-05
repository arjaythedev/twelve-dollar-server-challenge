-module(challenge_db).
-behaviour(gen_server).
-export([start_link/1, feed/0, post/1, create/3, like/2, health/0]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-define(POST_SQL, "SELECT p.id,p.body,p.created_at,u.username AS author,"
    "(SELECT count(*) FROM likes WHERE post_id=p.id) AS like_count "
    "FROM posts p JOIN users u ON u.id=p.user_id").
-define(POST_JSON, "json_object('id',id,'body',body,'created_at',created_at,"
    "'author',author,'like_count',like_count)").

start_link(Path) -> gen_server:start_link({local, ?MODULE}, ?MODULE, Path, []).
feed() -> gen_server:call(?MODULE, feed, infinity).
post(Id) -> gen_server:call(?MODULE, {post, Id}, infinity).
create(UserId, Body, Username) -> gen_server:call(?MODULE, {create, UserId, Body, Username}, infinity).
like(UserId, Id) -> gen_server:call(?MODULE, {like, UserId, Id}, infinity).
health() -> gen_server:call(?MODULE, health, infinity).

init(Path) ->
    {ok, Db} = esqlite3:open(Path),
    ok = esqlite3:exec(Db, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
        "PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;"),
    Sql = [
        {feed, ["SELECT json_object('posts',json_group_array(", ?POST_JSON, ")) FROM (",
            ?POST_SQL, " ORDER BY p.created_at DESC,p.id DESC LIMIT 20)"]},
        {post, ["SELECT json_object('post',", ?POST_JSON, ") FROM (", ?POST_SQL, " WHERE p.id=?)"]},
        {create, "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING "
            "json_object('post',json_object('id',id,'body',body,'created_at',created_at,"
            "'author',?3,'like_count',0))"},
        {like, "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS"
            "(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING"},
        {exists, "SELECT 1 FROM posts WHERE id=?"},
        {health, "SELECT 1"}
    ],
    Statements = maps:from_list([{Name, prepare(Db, Query)} || {Name, Query} <- Sql]),
    {ok, Statements#{db => Db}}.

prepare(Db, Sql) ->
    {ok, Statement} = esqlite3:prepare(Db, Sql, [{persistent, true}]),
    Statement.

handle_call(Request, _From, State) ->
    Reply = try execute(Request, State)
        catch _:_ ->
            case Request of
                health -> {error, <<"database query failed">>};
                _ -> internal_error()
            end
        end,
    {reply, Reply, State}.

execute(feed, State) -> read(feed, [], State);
execute({post, Id}, State) -> read(post, [Id], State);
execute({create, UserId, Body, Username}, State) ->
    with_statement(create, [UserId, Body, Username], State, fun(Statement) ->
        [Json] = esqlite3:step(Statement),
        '$done' = esqlite3:step(Statement), % Finish RETURNING and commit before replying.
        {201, Json}
    end);
execute({like, UserId, Id}, State = #{db := Db}) ->
    with_statement(like, [UserId, Id], State, fun(Statement) ->
        '$done' = esqlite3:step(Statement)
    end),
    case esqlite3:changes(Db) of
        1 -> liked(Id, false);
        0 -> with_statement(exists, [Id], State, fun(Statement) ->
            case esqlite3:step(Statement) of
                [1] -> liked(Id, true);
                '$done' -> not_found();
                _ -> internal_error()
            end
        end)
    end;
execute(health, State) ->
    with_statement(health, [], State, fun(Statement) ->
        case esqlite3:step(Statement) of
            [1] -> ok;
            _ -> {error, <<"database query failed">>}
        end
    end).

read(Name, Args, State) ->
    with_statement(Name, Args, State, fun(Statement) ->
        case esqlite3:step(Statement) of
            [Json] when is_binary(Json) -> {200, Json};
            '$done' -> not_found();
            _ -> internal_error()
        end
    end).

with_statement(Name, Args, State, Fun) ->
    Statement = maps:get(Name, State),
    try
        ok = esqlite3:bind(Statement, Args),
        Fun(Statement)
    after
        esqlite3:reset(Statement)
    end.

liked(Id, AlreadyLiked) ->
    {case AlreadyLiked of true -> 200; false -> 201 end,
        [<<"{\"liked\":true,\"already_liked\":" >>, atom_to_binary(AlreadyLiked),
            <<",\"post_id\":" >>, integer_to_binary(Id), <<"}">>]}.
not_found() -> {404, <<"{\"error\":\"post not found\"}">>}.
internal_error() -> {500, <<"{\"error\":\"internal server error\"}">>}.

handle_cast(_Request, State) -> {noreply, State}.
handle_info(_Message, State) -> {noreply, State}.
terminate(_Reason, #{db := Db}) -> esqlite3:close(Db).
code_change(_OldVersion, State, _Extra) -> {ok, State}.
