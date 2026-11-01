with Interfaces.C; use Interfaces.C;
with System;
with JSON;
with Sys;

package body Auth is
   use type System.Address;
   use type JSON.Value_Kind;

   function SHA256 return System.Address
     with Import, Convention => C, External_Name => "EVP_sha256";
   function HMAC_New return System.Address
     with Import, Convention => C, External_Name => "HMAC_CTX_new";
   function HMAC_Init (Context, Key : System.Address; Length : int;
                       Digest, Engine : System.Address) return int
     with Import, Convention => C, External_Name => "HMAC_Init_ex";
   function HMAC_Update (Context, Data : System.Address; Length : size_t) return int
     with Import, Convention => C, External_Name => "HMAC_Update";
   function HMAC_Final (Context, Digest : System.Address; Length : access unsigned)
                        return int
     with Import, Convention => C, External_Name => "HMAC_Final";
   function Constant_Compare (Left, Right : System.Address; N : size_t) return int
     with Import, Convention => C, External_Name => "CRYPTO_memcmp";

   -- Keyed once; each verification restarts from the keyed state and
   -- hashes the whole token again. No verification result is kept.
   Context : System.Address := System.Null_Address;

   Alg : aliased constant String := "alg";
   Sub : aliased constant String := "sub";
   Name : aliased constant String := "username";
   Exp : aliased constant String := "exp";
   Nbf : aliased constant String := "nbf";
   Header_Names : constant JSON.Name_Array := [1 => Alg'Access];
   Claim_Names : constant JSON.Name_Array :=
     [Sub'Access, Name'Access, Exp'Access, Nbf'Access];

   -- Decoded JOSE header and claims of the current token.
   Header_Text : String (1 .. 12_300);
   Payload_Text : String (1 .. 12_300);

   Bad_Token : exception;

   procedure Initialize (Secret : String) is
      Empty : aliased constant String := " ";
   begin
      Context := HMAC_New;
      if Context = System.Null_Address or else
        HMAC_Init (Context, (if Secret'Length = 0 then Empty'Address else Secret'Address),
                   int (Secret'Length), SHA256, System.Null_Address) /= 1
      then
         raise Program_Error with "HMAC initialization failed";
      end if;
   end Initialize;

   procedure Decode (Encoded : String; Target : in out String; Length : out Natural) is
      Count, Bits, Accumulator, Value : Natural := 0;
   begin
      if Encoded'Length = 0 or else Encoded'Length mod 4 = 1 then
         raise Bad_Token;
      end if;
      for C of Encoded loop
         case C is
            when 'A' .. 'Z' => Value := Character'Pos (C) - 65;
            when 'a' .. 'z' => Value := Character'Pos (C) - 71;
            when '0' .. '9' => Value := Character'Pos (C) + 4;
            when '-' => Value := 62;
            when '_' => Value := 63;
            when others => raise Bad_Token;
         end case;
         Accumulator := Accumulator * 64 + Value;
         Bits := Bits + 6;
         if Bits >= 8 then
            Bits := Bits - 8;
            Target (Target'First + Count) := Character'Val (Accumulator / 2 ** Bits);
            Count := Count + 1;
            Accumulator := Accumulator mod 2 ** Bits;
         end if;
      end loop;
      if Accumulator /= 0 then
         raise Bad_Token;
      end if;
      Length := Count;
   end Decode;

   function Find_Dot (Text : String; From : Positive) return Natural is
   begin
      for I in From .. Text'Last loop
         if Text (I) = '.' then
            return I;
         end if;
      end loop;
      return 0;
   end Find_Dot;

   procedure Verify (Has_Header : Boolean; Header : String; Result : out Status;
                     User_ID : out Long_Long_Integer) is
      First, Second : Natural;
      Digest : aliased String (1 .. 32);
      Signature : aliased String (1 .. 33);
      N : aliased unsigned := 0;
      Header_Length, Payload_Length, Signature_Length : Natural := 0;
      Header_Fields : JSON.Field_Array (1 .. 1);
      Claims : JSON.Field_Array (1 .. 4);
      Root : JSON.Value_Kind;
      Now : Long_Long_Float;
   begin
      User_ID := 0;
      Username_Length := 0;
      Result := Missing_Bearer;
      if not Has_Header or else Header'Length < 7 or else
        Header (Header'First .. Header'First + 6) /= "Bearer "
      then
         return;
      end if;
      Result := Invalid_Token;
      declare
         Token : String renames Header (Header'First + 7 .. Header'Last);
      begin
         First := Find_Dot (Token, Token'First);
         if First = 0 then return; end if;
         Second := Find_Dot (Token, First + 1);
         if Second = 0 or else Second = Token'Last or else Find_Dot (Token, Second + 1) /= 0 then
            return;
         end if;
         -- 32 bytes of HS256 signature are exactly 43 base64url characters.
         if Token'Last - Second /= 43 then return; end if;
         Decode (Token (Second + 1 .. Token'Last), Signature, Signature_Length);
         if Signature_Length /= 32 or else
           HMAC_Init (Context, System.Null_Address, 0, System.Null_Address,
                      System.Null_Address) /= 1 or else
           HMAC_Update (Context, Token (Token'First)'Address,
                        size_t (Second - Token'First)) /= 1 or else
           HMAC_Final (Context, Digest'Address, N'Access) /= 1 or else
           N /= 32 or else
           Constant_Compare (Digest'Address, Signature'Address, 32) /= 0
         then
            return;
         end if;
         Decode (Token (Token'First .. First - 1), Header_Text, Header_Length);
         JSON.Parse (Header_Text (1 .. Header_Length), Header_Names, Header_Fields, Root);
         if Root /= JSON.Object_Value or else
           Header_Fields (1).Kind /= JSON.String_Value or else
           not JSON.Equals (Header_Text (1 .. Header_Length), Header_Fields (1), "HS256")
         then
            return;
         end if;
         Decode (Token (First + 1 .. Second - 1), Payload_Text, Payload_Length);
      end;
      declare
         Payload : String renames Payload_Text (1 .. Payload_Length);
      begin
         JSON.Parse (Payload, Claim_Names, Claims, Root);
         Now := Sys.Unix_Time;
         if Root /= JSON.Object_Value or else Claims (3).Kind /= JSON.Number_Value
           or else Long_Long_Float'Value (Payload (Claims (3).First .. Claims (3).Last)) <= Now
         then
            return;
         end if;
         if Claims (4).Kind /= JSON.Missing and then
           (Claims (4).Kind /= JSON.Number_Value or else
            Long_Long_Float'Value (Payload (Claims (4).First .. Claims (4).Last)) > Now)
         then
            return;
         end if;
         Result := Invalid_Payload;
         if Claims (1).Kind /= JSON.String_Value or else
           Claims (2).Kind /= JSON.String_Value
         then
            return;
         end if;
         declare
            Subject : String (1 .. Claims (1).Last - Claims (1).First + 1);
            Subject_Length : Natural;
         begin
            JSON.Decode (Payload, Claims (1), Subject, Subject_Length);
            User_ID := JSON.Positive_ID (Subject (1 .. Subject_Length));
         end;
         if User_ID = 0 then return; end if;
         JSON.Decode (Payload, Claims (2), Username, Username_Length);
         Result := Valid;
      end;
   exception
      when Bad_Token | JSON.Invalid_JSON | Constraint_Error =>
         Result := Invalid_Token;
         User_ID := 0;
   end Verify;
end Auth;
