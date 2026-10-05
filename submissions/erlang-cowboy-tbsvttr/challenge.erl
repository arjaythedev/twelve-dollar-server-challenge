-module(challenge).
-export([start/0, init/2]).

start() ->
    Path = required_env("SQLITE_PATH"),
    Secret = list_to_binary(required_env("JWT_SECRET")),
    {ok, _} = application:ensure_all_started(crypto),
    {ok, _} = application:ensure_all_started(cowboy),
    {ok, _} = challenge_db:start_link(Path),
    {ok, Address} = inet:getaddr(os:getenv("HOST", "127.0.0.1"), inet),
    Port = list_to_integer(os:getenv("PORT", "3000")),
    State = #{secret => Secret, started => erlang:monotonic_time(second)},
    Dispatch = cowboy_router:compile([{'_', [{"/[...]", ?MODULE, State}]}]),
    {ok, _} = cowboy:start_clear(challenge_listener,
        #{socket_opts => [{ip, Address}, {port, Port}, {nodelay, true}],
          num_acceptors => 10, max_connections => 20000},
        #{env => #{dispatch => Dispatch}, protocols => [http],
          idle_timeout => 75000, request_timeout => 75000, inactivity_timeout => 75000,
          max_keepalive => 1000000000, max_header_value_length => 16384,
          max_authorization_header_value_length => 16384}),
    io:format("Listening on ~s:~B~n", [inet:ntoa(Address), Port]).

required_env(Name) ->
    case os:getenv(Name) of
        Value when is_list(Value), Value =/= [] -> Value;
        _ -> erlang:error({missing_environment_variable, Name})
    end.

init(Req0, State) ->
    {Status, Body, Req} = try route(cowboy_req:method(Req0),
                                  cowboy_req:path(Req0), Req0, State)
    catch
        throw:{http_error, Code, Message} -> {Code, error_json(Message), Req0};
        Class:Reason ->
            logger:error("Request failed: ~p:~p", [Class, Reason]),
            {500, error_json(<<"internal server error">>), Req0}
    end,
    {ok, cowboy_req:reply(Status, #{<<"content-type">> => <<"application/json">>}, Body, Req), State}.

route(<<"GET">>, <<"/feed">>, Req, _) -> respond(challenge_db:feed(), Req);
route(<<"GET">>, <<"/health">>, Req, #{started := Started}) ->
    Health = try challenge_db:health() catch _:_ -> {error, <<"database unavailable">>} end,
    case Health of
        ok ->
            Uptime = integer_to_binary(erlang:monotonic_time(second) - Started),
            {200, [<<"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" >>, Uptime, $}], Req};
        Failure ->
            Message = case Failure of
                {error, Error} when is_binary(Error) -> Error;
                _ -> <<"database unavailable">>
            end,
            {503, [<<"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":" >>,
                   json:encode(Message), $}], Req}
    end;
route(<<"POST">>, <<"/posts">>, Req0, #{secret := Secret}) ->
    {UserId, Username} = challenge_auth:verify(cowboy_req:header(<<"authorization">>, Req0), Secret),
    {Raw, Req} = read_body(Req0, [], 0),
    respond(challenge_db:create(UserId, post_body(Raw), Username), Req);
route(Method, <<"/posts/", Rest/binary>>, Req, #{secret := Secret}) ->
    case {Method, binary:split(Rest, <<"/">>, [global])} of
        {<<"GET">>, [Id]} -> respond(challenge_db:post(post_id(Id)), Req);
        {<<"POST">>, [Id, <<"like">>]} ->
            {UserId, _} = challenge_auth:verify(cowboy_req:header(<<"authorization">>, Req), Secret),
            respond(challenge_db:like(UserId, post_id(Id)), Req);
        _ -> fail(404, <<"not found">>)
    end;
route(_, _, _, _) -> fail(404, <<"not found">>).

respond({Status, Body}, Req) -> {Status, Body, Req}.
error_json(Message) -> [<<"{\"error\":" >>, json:encode(Message), $}].
fail(Status, Message) -> throw({http_error, Status, Message}).
post_id(Binary) ->
    case challenge_auth:positive_id(Binary) of
        0 -> fail(400, <<"invalid post id">>);
        Id -> Id
    end.

read_body(Req0, Chunks, Size) ->
    {More, Chunk, Req} = cowboy_req:read_body(Req0, #{length => 16385, period => 75000}),
    Total = Size + byte_size(Chunk),
    case Total > 16384 of true -> fail(413, <<"request body too large">>); false -> ok end,
    case More of
        ok -> {iolist_to_binary(lists:reverse([Chunk | Chunks])), Req};
        more -> read_body(Req, [Chunk | Chunks], Total)
    end.

post_body(Raw) ->
    Json = try json:decode(Raw) catch error:_ -> fail(400, <<"malformed JSON body">>) end,
    Value = case Json of
        #{<<"body">> := Body} when is_binary(Body) -> unicode:characters_to_list(Body);
        _ -> fail(400, <<"body is required">>)
    end,
    % Trim Unicode code points, not graphemes: this matches JavaScript String.trim().
    Trimmed = lists:reverse(lists:dropwhile(fun space/1,
                            lists:reverse(lists:dropwhile(fun space/1, Value)))),
    case length(Trimmed) of
        0 -> fail(400, <<"body is required">>);
        N when N > 500 -> fail(400, <<"body must be at most 500 characters">>);
        _ -> unicode:characters_to_binary(Trimmed)
    end.

space(C) when C >= 9, C =< 13 -> true;
space(C) when C >= 16#2000, C =< 16#200A -> true;
space(C) -> lists:member(C, [32, 16#A0, 16#1680, 16#2028, 16#2029,
                           16#202F, 16#205F, 16#3000, 16#FEFF]).
