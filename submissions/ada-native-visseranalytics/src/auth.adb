with Ada.Strings.Fixed;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces.C; use Interfaces.C;
with System;
with JSON;

package body Auth is
   use type System.Address;
   use type JSON.Value_Kind;

   function SHA256 return System.Address
     with Import, Convention => C, External_Name => "EVP_sha256";
   function HMAC (Algorithm, Key : System.Address; Key_Length : int;
                  Data : System.Address; Data_Length : size_t;
                  Digest : System.Address; Digest_Length : access unsigned)
                  return System.Address
     with Import, Convention => C, External_Name => "HMAC";
   function Constant_Compare (Left, Right : System.Address; N : size_t) return int
     with Import, Convention => C, External_Name => "CRYPTO_memcmp";
   type Timespec is record
      Seconds, Nanoseconds : long;
   end record with Convention => C;
   function Get_Time (Clock_ID : int; Stamp : access Timespec) return int
     with Import, Convention => C, External_Name => "clock_gettime";

   function Unix_Time return Long_Long_Float is
      Stamp : aliased Timespec;
      RC : constant int := Get_Time (0, Stamp'Access);
   begin
      if RC /= 0 then raise Program_Error with "clock_gettime failed"; end if;
      return Long_Long_Float (Stamp.Seconds) +
        Long_Long_Float (Stamp.Nanoseconds) / 1_000_000_000.0;
   end Unix_Time;

   Bad_Token : exception;
   function Decode (Encoded : String) return String is
      Buffer : String (1 .. Encoded'Length * 3 / 4 + 1);
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
         Accumulator := Accumulator * 64 + Value; Bits := Bits + 6;
         if Bits >= 8 then
            Bits := Bits - 8; Count := Count + 1;
            Buffer (Count) := Character'Val (Accumulator / 2 ** Bits);
            Accumulator := Accumulator mod 2 ** Bits;
         end if;
      end loop;
      if Accumulator /= 0 then raise Bad_Token; end if;
      return Buffer (1 .. Count);
   end Decode;

   procedure Verify (Authorization, Secret : String; Result : out Status;
                     User_ID : out Long_Long_Integer;
                     Username : out Unbounded_String) is
      use Ada.Strings.Fixed;
      Header : JSON.Fields :=
        (1 => (To_Unbounded_String ("alg"), JSON.Missing, Null_Unbounded_String));
      Claims : JSON.Fields :=
        ((To_Unbounded_String ("sub"), JSON.Missing, Null_Unbounded_String),
         (To_Unbounded_String ("username"), JSON.Missing, Null_Unbounded_String),
         (To_Unbounded_String ("exp"), JSON.Missing, Null_Unbounded_String),
         (To_Unbounded_String ("nbf"), JSON.Missing, Null_Unbounded_String));
      Root : JSON.Value_Kind;
      First, Second : Natural;
      Digest : aliased String (1 .. 32);
      N : aliased unsigned := 0;
      Pointer : System.Address;
      Now : constant Long_Long_Float := Unix_Time;
   begin
      User_ID := 0; Username := Null_Unbounded_String;
      Result := Missing_Bearer;
      if Authorization'Length < 7 or else
        Authorization (Authorization'First .. Authorization'First + 6) /= "Bearer "
      then
         return;
      end if;
      Result := Invalid_Token;
      declare
         Token : constant String :=
           Authorization (Authorization'First + 7 .. Authorization'Last);
      begin
         First := Index (Token, ".");
         if First = 0 then return; end if;
         Second := Index (Token, ".", First + 1);
         if Second = 0 or else Second = Token'Last or else
           Index (Token, ".", Second + 1) /= 0
         then
            return;
         end if;
         declare
            Signature : constant String := Decode (Token (Second + 1 .. Token'Last));
            Signed : constant String := Token (Token'First .. Second - 1);
         begin
            if Signature'Length /= 32 then return; end if;
            Pointer := HMAC (SHA256, Secret'Address, int (Secret'Length),
                             Signed'Address, size_t (Signed'Length),
                             Digest'Address, N'Access);
            if Pointer = System.Null_Address or else N /= 32 or else
              Constant_Compare (Digest'Address, Signature'Address, 32) /= 0
            then
               return;
            end if;
         end;
         JSON.Parse (Decode (Token (Token'First .. First - 1)), Header, Root);
         if Root /= JSON.Object_Value or else
           Header (1).Kind /= JSON.String_Value or else Header (1).Text /= "HS256"
         then
            return;
         end if;
         JSON.Parse (Decode (Token (First + 1 .. Second - 1)), Claims, Root);
      end;
      if Root /= JSON.Object_Value or else Claims (3).Kind /= JSON.Number_Value
        or else Long_Long_Float'Value (To_String (Claims (3).Text)) <= Now
      then
         return;
      end if;
      if Claims (4).Kind /= JSON.Missing and then
        (Claims (4).Kind /= JSON.Number_Value or else
         Long_Long_Float'Value (To_String (Claims (4).Text)) > Now)
      then
         return;
      end if;
      Result := Invalid_Payload;
      if Claims (1).Kind /= JSON.String_Value or else
        Claims (2).Kind /= JSON.String_Value
      then
         return;
      end if;
      User_ID := JSON.Positive_ID (To_String (Claims (1).Text));
      if User_ID = 0 then return; end if;
      Username := Claims (2).Text;
      Result := Valid;
   exception
      when Bad_Token | JSON.Invalid_JSON | Constraint_Error =>
         Result := Invalid_Token;
   end Verify;
end Auth;
