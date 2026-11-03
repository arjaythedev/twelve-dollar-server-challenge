-module(challenge_auth).
-export([verify/2, positive_id/1]).

verify(<<"Bearer ", Token/binary>>, Secret) ->
    Payload = try
        [Header64, Payload64, Signature64] = binary:split(Token, <<".">>, [global]),
        43 = byte_size(Signature64),
        Signature = decode64(Signature64),
        Expected = crypto:mac(hmac, sha256, Secret, [Header64, $., Payload64]),
        true = crypto:hash_equals(Expected, Signature),
        #{<<"alg">> := <<"HS256">>} = json:decode(decode64(Header64)),
        #{<<"exp">> := Expiry} = Claims = json:decode(decode64(Payload64)),
        Now = erlang:system_time(microsecond) / 1000000,
        true = is_number(Expiry) andalso Expiry =< 1.7976931348623157e308 andalso Expiry > Now,
        NotBefore = maps:get(<<"nbf">>, Claims, 0),
        true = is_number(NotBefore) andalso NotBefore >= -1.7976931348623157e308 andalso NotBefore =< Now,
        Claims
    catch _:_ -> fail(<<"invalid or expired token">>) end,
    case Payload of
        #{<<"sub">> := Subject, <<"username">> := Username} when is_binary(Username) ->
            case positive_id(Subject) of
                0 -> fail(<<"invalid token payload">>);
                Id -> {Id, Username}
            end;
        _ -> fail(<<"invalid token payload">>)
    end;
verify(_, _) -> fail(<<"missing bearer token">>).

fail(Message) -> throw({http_error, 401, Message}).

% OTP's decoder accepts whitespace and padding; JWT segments must contain neither.
decode64(Binary) when byte_size(Binary) > 0 ->
    true = valid64(Binary),
    base64:decode(Binary, #{mode => urlsafe, padding => false}).
valid64(<<>>) -> true;
valid64(<<C, Rest/binary>>) when C >= $a, C =< $z; C >= $A, C =< $Z;
                                    C >= $0, C =< $9; C =:= $-; C =:= $_ -> valid64(Rest);
valid64(_) -> false.

positive_id(<<C, Rest/binary>>) when C >= $0, C =< $9 -> digits(Rest, C - $0);
positive_id(_) -> 0.
digits(_, N) when N > 9007199254740991 -> 0;
digits(<<>>, N) -> N;
digits(<<C, Rest/binary>>, N) when C >= $0, C =< $9 -> digits(Rest, N * 10 + C - $0);
digits(_, _) -> 0.
