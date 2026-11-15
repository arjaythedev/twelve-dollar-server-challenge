% The whole API in SWI-Prolog: one thread, one event loop over wait_for_input/3, one SQLite connection.
% Run with: swipl server.pl   (config from SQLITE_PATH, JWT_SECRET, HOST, PORT)
:- use_module(library(socket)).
:- use_module(library(sha)).
:- use_module(library(base64)).
:- use_module(library(utf8)).
:- use_module(library(lists)).
:- prolog_load_context(directory, Dir), atom_concat(Dir, '/lib', Lib), asserta(user:file_search_path(foreign, Lib)).
:- use_foreign_library(foreign(sqlite4pl)).   % built by build.sh from sqlite4pl.c

:- initialization(main, main).

:- dynamic conn/2, rest/2, dirty/0, stmt/2, started/1.

% ---------------------------------------------------------------- SQL
% A post object is built by SQLite's json_object, in the spec's key order.
post_json("json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, \c
           'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))").

sql(feed, Q) :- post_json(J), format(string(Q),
    "SELECT '{\"posts\":[' || ifnull(group_concat(j, ',' ORDER BY c DESC, id DESC), '') || ']}' \c
     FROM (SELECT p.id, p.created_at AS c, ~w AS j FROM posts p JOIN users u ON u.id = p.user_id \c
           ORDER BY p.created_at DESC, p.id DESC LIMIT 20)", [J]).
sql(post, Q) :- post_json(J), format(string(Q),
    "SELECT ~w FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1", [J]).
sql(create, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING json_object('id', id, 'body', body, \c
             'created_at', created_at, 'author', ?3, 'like_count', 0)").
sql(like, "INSERT INTO likes (user_id, post_id) SELECT ?1, id FROM posts WHERE id = ?2 \c
           ON CONFLICT (user_id, post_id) DO NOTHING").
sql(exists, "SELECT 1 FROM posts WHERE id = ?1").
sql(health, "SELECT 1").
sql(begin, "BEGIN IMMEDIATE").
sql(commit, "COMMIT").
sql(rollback, "ROLLBACK").

pragma("PRAGMA journal_mode=WAL").
pragma("PRAGMA synchronous=NORMAL").
pragma("PRAGMA busy_timeout=5000").
pragma("PRAGMA cache_size=-16000").
pragma("PRAGMA mmap_size=268435456").

run(Name, Params, Result) :- stmt(Name, S), sqlite_run(S, Params, Result).

% ---------------------------------------------------------------- routes
% route(+Method, +PathSegments, +Request, -Response). Response is Status-Body, Body is bytes.
route("GET", ["feed"], _, R) :- !,
    ( run(feed, [], row(B)) -> R = 200-B ; R = 500-"" ).
route("GET", ["health"], _, R) :- !,
    run(health, [], Res),
    (   Res = row(_)
    ->  started(T0), get_time(T), Up is floor(T - T0),
        format(string(B), "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":~d}", [Up]), R = 200-B
    ;   Res = error(Msg), json_quote(Msg, Q),
        format(string(B), "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":~w}", [Q]), R = 503-B
    ).
route("GET", ["posts", Id], _, R) :- Id \== "", !,
    (   post_id(Id, N)
    ->  run(post, [N], Res),
        (   Res = row(P) -> string_concat("{\"post\":", P, B0), string_concat(B0, "}", B), R = 200-B
        ;   Res = done(_) -> R = error(404, "post not found")
        ;   R = 500-""
        )
    ;   R = error(400, "invalid post id")
    ).
route("POST", ["posts"], Req, R) :- !,
    authorized(Req, R, Uid, Name),
    ( var(R) -> create(Req, Uid, Name, R) ; true ).
route("POST", ["posts", Id, "like"], Req, R) :- Id \== "", !,
    authorized(Req, R, Uid, _),
    (   nonvar(R) -> true
    ;   post_id(Id, N) -> like(Uid, N, R)
    ;   R = error(400, "invalid post id")
    ).
route(_, _, _, error(404, "not found")).

create(req(_, Body), Uid, Name, R) :-
    string_codes(Body, Bytes),
    (   phrase(utf8_codes(Cs), Bytes), phrase(json(V), Cs)
    ->  (   V = obj(Ps), last_value(Ps, "body", Text), string(Text),
            string_codes(Text, Tc), trim(Tc, Trimmed), Trimmed \== []
        ->  (   length(Trimmed, Len), Len > 500
            ->  R = error(400, "body must be at most 500 characters")
            ;   string_codes(T, Trimmed), write_sql(create, [Uid, T, Name], Res),
                (   Res = row(P) -> string_concat("{\"post\":", P, B0), string_concat(B0, "}", B), R = 201-B
                ;   R = 500-""
                )
            )
        ;   R = error(400, "body is required")
        )
    ;   R = error(400, "malformed JSON body")
    ).

like(Uid, N, R) :-
    write_sql(like, [Uid, N], Res),
    (   Res = done(1) -> Status = 201, Already = false
    ;   Res = done(0), run(exists, [N], E) ->
        (   E = row(_) -> Status = 200, Already = true
        ;   E = done(_) -> R = error(404, "post not found")
        ;   R = 500-""
        )
    ;   R = 500-""
    ),
    (   var(R)
    ->  format(string(B), "{\"liked\":true,\"already_liked\":~w,\"post_id\":~d}", [Already, N]), R = Status-B
    ;   true
    ).

% The first write of a loop turn opens a transaction; flush/0 commits it before any response is sent.
write_sql(Name, Params, Res) :-
    (   dirty -> Begun = done(0) ; run(begin, [], Begun), assertz(dirty) ),
    ( Begun = done(_) -> run(Name, Params, Res) ; Res = Begun ).

% A positive integer of ASCII digits. One too large for SQLite becomes an id no row has.
post_id(S, N) :-
    string_codes(S, Cs), Cs \== [], forall(member(C, Cs), between(0'0, 0'9, C)),
    number_codes(N0, Cs), N0 > 0, N is min(N0, 9223372036854775807).

% ---------------------------------------------------------------- auth
% authorized(+Request, -Error, -UserId, -Username): Error stays unbound when the token is good.
authorized(req(Head, _), R, Uid, Name) :-
    (   header(Head, "authorization", V), string_concat("Bearer ", Tok, V)
    ->  (   verified(Tok, Payload)
        ->  (   last_value(Payload, "sub", Sub), string(Sub), string_length(Sub, L), L < 19, post_id(Sub, Uid),
                last_value(Payload, "username", Name), string(Name)
            ->  true
            ;   R = error(401, "invalid token payload")
            )
        ;   R = error(401, "invalid or expired token")
        )
    ;   R = error(401, "missing bearer token")
    ).

verified(Tok, Payload) :-
    split_string(Tok, ".", "", [H, P, S]),
    b64_json(H, obj(Hd)), last_value(Hd, "alg", "HS256"),
    secret(Key), string_length(H, HL), string_length(P, PL), DL is HL + 1 + PL,
    sub_string(Tok, 0, DL, _, Data),
    hmac_sha(Key, Data, Mac, [algorithm(sha256)]),
    unbase64(S, Sig), string_codes(Sig, Mac),
    b64_json(P, obj(Payload)),
    get_time(Now),
    last_value(Payload, "exp", Exp), number(Exp), Exp > floor(Now),
    ( last_value(Payload, "nbf", Nbf) -> number(Nbf), Nbf =< floor(Now) ; true ).

b64_json(B64, V) :-
    unbase64(B64, Plain),
    string_codes(Plain, Bytes), phrase(utf8_codes(Cs), Bytes), phrase(json(V), Cs).

secret(K) :- getenv('JWT_SECRET', K).

unbase64(B64, Bytes) :- % base64url without padding, to a string of bytes; fails on anything else
    catch(base64_encoded(Bytes, B64, [charset(url), padding(false), as(string), encoding(octet)]), _, fail).

% ---------------------------------------------------------------- strict JSON (RFC 8259) as a DCG
% Objects become obj([Key-Value, ...]) with keys in order, strings are strings, numbers are numbers.
json(V) --> ws, value(V), ws.

value(obj(Ps)) --> "{", !, ws, ( "}" -> { Ps = [] } ; members(Ps) ).
value(Vs) --> "[", !, ws, ( "]" -> { Vs = [] } ; elements(Vs) ).
value(S) --> "\"", !, chars(Cs), { string_codes(S, Cs) }.
value(true) --> "true", !.
value(false) --> "false", !.
value(null) --> "null", !.
value(N) --> number(N).

members([K-V|Ps]) --> "\"", chars(Kc), { string_codes(K, Kc) }, ws, ":", json(V),
    ( "," -> ws, members(Ps) ; "}", { Ps = [] } ).
elements([V|Vs]) --> json(V), ( "," -> elements(Vs) ; "]", { Vs = [] } ).

chars([]) --> "\"", !.
chars([C|Cs]) --> "\\", !, escape(C), chars(Cs).
chars([C|Cs]) --> [C], { C >= 0x20 }, chars(Cs).

escape(0'") --> "\"".
escape(0'\\) --> "\\".
escape(0'/) --> "/".
escape(8) --> "b".
escape(12) --> "f".
escape(10) --> "n".
escape(13) --> "r".
escape(9) --> "t".
escape(C) --> "u", hex4(H),
    (   { H >= 0xD800, H =< 0xDBFF } -> "\\u", hex4(L), { L >= 0xDC00, L =< 0xDFFF, C is 0x10000 + (H - 0xD800) * 0x400 + L - 0xDC00 }
    ;   { H < 0xDC00 ; H > 0xDFFF }, { C = H }   % shortcut: lone surrogates are rejected, not passed through
    ).

hex4(H) --> hex(A), hex(B), hex(C), hex(D), { H is A << 12 + B << 8 + C << 4 + D }.
hex(V) --> [C], { code_type(C, xdigit(V)) }.

number(N) --> optional_minus(S), integer_part(I), fraction(F), exponent(E),
    { F == [], E \== [] -> F1 = `.0` ; F1 = F },
    { append([S, I, F1, E], Cs), catch(number_codes(N, Cs), _, fail) }.
optional_minus(`-`) --> "-", !.
optional_minus([]) --> [].
integer_part([0'0]) --> "0", !.
integer_part([D|Ds]) --> [D], { between(0'1, 0'9, D) }, digits(Ds).
fraction([0'., D|Ds]) --> ".", !, digit(D), digits(Ds).
fraction([]) --> [].
exponent([0'e|Cs]) --> ( "e" ; "E" ), !, ( "-" -> { S = `-` } ; "+" -> { S = [] } ; { S = [] } ),
    digit(D), digits(Ds), { append(S, [D|Ds], Cs) }.
exponent([]) --> [].
digits([D|Ds]) --> digit(D), !, digits(Ds).
digits([]) --> [].
digit(D) --> [D], { between(0'0, 0'9, D) }.

ws --> [C], { C == 0' ; C == 0'\t ; C == 0'\n ; C == 0'\r }, !, ws.
ws --> [].

last_value(Pairs, K, V) :- findall(V0, member(K-V0, Pairs), Vs), last(Vs, V).

trim(Cs, T) :- drop_space(Cs, C1), reverse(C1, R1), drop_space(R1, R2), reverse(R2, T).
drop_space([C|Cs], T) :- code_type(C, space), C < 128, !, drop_space(Cs, T).
drop_space(Cs, Cs).

json_quote(S, Q) :- % for the one message we do not get from json_object
    string_codes(S, Cs), foldl([C, A0, A]>>( memberchk(C, `"\\`) -> append(A0, [0'\\, C], A)
                                            ; C < 32 -> format(codes(E), "\\u~|~`0t~16r~4+", [C]), append(A0, E, A)
                                            ; append(A0, [C], A) ), Cs, [], Q0),
    format(string(Q), "\"~s\"", [Q0]).

% ---------------------------------------------------------------- HTTP/1.1
header(Head, Name, Value) :- % the first header line called Name (any case)
    split_string(Head, "\n", "\r", [_|Lines]), member(Line, Lines),
    once(sub_string(Line, B, 1, A, ":")),
    sub_string(Line, 0, B, _, N0), string_lower(N0, Name), !,
    sub_string(Line, _, A, 0, V0), split_string(V0, "", " \t", [Value]).

status_line(200, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(201, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(400, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(401, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(404, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(500, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ").
status_line(503, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ").

send(O, error(Status, Msg)) :- !, atomics_to_string(["{\"error\":\"", Msg, "\"}"], B), send(O, Status-B).
send(O, 500-"") :- !, send(O, error(500, "internal server error")).
send(O, Status-Body) :-
    status_line(Status, H), string_length(Body, Len),
    write(O, H), write(O, Len), write(O, "\r\n\r\n"), write(O, Body).

% Answers every complete request in Buf, in order; returns what is left of an unfinished one.
requests(Buf, Rs, Left) :-
    (   sub_string(Buf, HE, 4, _, "\r\n\r\n")
    ->  sub_string(Buf, 0, HE, _, Head),
        ( sub_string(Head, LE, 2, _, "\r\n") -> true ; string_length(Head, LE) ),
        sub_string(Head, 0, LE, _, Line),
        split_string(Line, " ", "", [Method, Target|_]),
        BodyStart is HE + 4,
        (   Method == "GET" -> CL = 0
        ;   header(Head, "content-length", V), number_string(CL, V), integer(CL), CL >= 0 -> true
        ;   CL = 0
        ),
        string_length(Buf, Len),
        (   BodyStart + CL > Len -> Rs = [], Left = Buf
        ;   sub_string(Buf, BodyStart, CL, After, Body),
            ( sub_string(Target, Q, 1, _, "?") -> sub_string(Target, 0, Q, _, Path) ; Path = Target ),
            ( split_string(Path, "/", "", [""|Segs]) -> true ; Segs = [] ),
            ( catch(route(Method, Segs, req(Head, Body), R), E, (print_message(error, E), fail)) -> true ; R = 500-"" ),
            Rs = [R|Rs1],
            sub_string(Buf, _, After, 0, Rest),
            requests(Rest, Rs1, Left)
        )
    ;   Rs = [], Left = Buf
    ).

% ---------------------------------------------------------------- event loop
main :-
    getenv('SQLITE_PATH', Path), sqlite_open(Path),
    forall(pragma(P), ( sqlite_prepare(P, S), sqlite_run(S, [], Res), Res \= error(_) )),
    forall(sql(Name, Q), ( sqlite_prepare(Q, S), assertz(stmt(Name, S)) )),
    getenv('HOST', Host), getenv('PORT', PortA), atom_number(PortA, Port),
    tcp_socket(Sock), tcp_setopt(Sock, reuseaddr), tcp_bind(Sock, Host:Port), tcp_listen(Sock, 4096),
    tcp_open_socket(Sock, Listen, _),
    get_time(T0), assertz(started(T0)),
    format(user_error, "listening on ~w:~w~n", [Host, Port]),
    repeat,   % failure driven: each turn starts with empty stacks, connection state lives in the database
    catch(turn(Sock, Listen), E, print_message(error, E)),
    fail.

turn(Sock, Listen) :-
    findall(I, conn(I, _), Ins),   % O(connections) per turn: fine for Nginx's few dozen, not for 15,000
    wait_for_input([Listen|Ins], Ready, infinite),
    foldl(ready(Sock, Listen), Ready, [], Outs),
    flush(Outs).

ready(Sock, Listen, I, Outs0, Outs) :-
    (   I == Listen -> accept_all(Sock, Listen, 256), Outs = Outs0
    ;   readable(I, Outs0, Outs)
    ).

accept_all(Sock, Listen, N) :- % what the backlog holds, at most N per turn
    catch(accept(Sock), E, print_message(error, E)),
    (   N > 1, wait_for_input([Listen], [_], 0) -> N1 is N - 1, accept_all(Sock, Listen, N1) ; true ).

accept(Sock) :-
    tcp_accept(Sock, C, _),
    tcp_open_socket(C, I, O),
    catch(tcp_setopt(C, nodelay), _, true),   % fails on a peer that already reset; the read will drop it
    set_stream(I, encoding(octet)), set_stream(O, encoding(octet)), set_stream(O, buffer_size(65536)),
    assertz(conn(I, O)).

readable(I, Outs0, Outs) :- % adds Out-Responses for the requests that arrived on I
    conn(I, O),
    (   catch((fill_buffer(I), read_pending_codes(I, Cs, [])), _, fail), Cs \== []
    ->  string_codes(New, Cs),
        ( retract(rest(I, Old)) -> string_concat(Old, New, Buf) ; Buf = New ),
        requests(Buf, Rs, Left),
        ( Rs == [] -> Outs = Outs0 ; Outs = [O-Rs|Outs0] ),
        string_length(Left, LL),
        (   LL > 1048576 -> drop(I)            % no request of ours is this large
        ;   LL > 0 -> assertz(rest(I, Left))
        ;   true
        )
    ;   drop(I), Outs = Outs0
    ).

drop(I) :-
    retract(conn(I, O)), retractall(rest(I, _)),
    close(I, [force(true)]), close(O, [force(true)]).

% Group commit: one COMMIT for every write of this turn, then the responses. A 201 never leaves first.
% If the COMMIT fails, nothing of this turn was written, and those clients get no answer at all.
flush(Outs) :-
    (   retract(dirty)
    ->  ( run(commit, [], done(_)) -> Ok = true ; run(rollback, [], _), Ok = false )
    ;   Ok = true
    ),
    forall(member(O-Rs, Outs),
           (   Ok == true, catch(( forall(member(R, Rs), send(O, R)), flush_output(O) ), _, fail) -> true
           ;   conn(I, O) -> drop(I)
           ;   true
           )).
