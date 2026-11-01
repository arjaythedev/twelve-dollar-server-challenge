package Auth is
   type Status is (Valid, Missing_Bearer, Invalid_Token, Invalid_Payload);
   procedure Initialize (Secret : String);
   -- Verifies an Authorization header value (HS256 signature, alg, exp, nbf)
   -- on every call. A Valid result leaves the token's username, unescaped,
   -- in Username (1 .. Username_Length).
   procedure Verify (Has_Header : Boolean; Header : String; Result : out Status;
                     User_ID : out Long_Long_Integer);
   Username : String (1 .. 16_384);
   Username_Length : Natural := 0;
end Auth;
