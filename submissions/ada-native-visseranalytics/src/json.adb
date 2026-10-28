with Ada.Strings.Fixed;

package body JSON is
   procedure Decode (Text : String; Position : in out Integer;
                     Code : out Natural) is
      First : Natural;
      Count : Natural;
      Minimum : Natural;
   begin
      if Position > Text'Last then
         raise Invalid_JSON;
      end if;
      First := Character'Pos (Text (Position));
      Position := Position + 1;
      if First < 128 then
         Code := First;
         return;
      elsif First in 16#C2# .. 16#DF# then
         Code := First - 16#C0#; Count := 1; Minimum := 16#80#;
      elsif First in 16#E0# .. 16#EF# then
         Code := First - 16#E0#; Count := 2; Minimum := 16#800#;
      elsif First in 16#F0# .. 16#F4# then
         Code := First - 16#F0#; Count := 3; Minimum := 16#10000#;
      else
         raise Invalid_JSON;
      end if;
      for I in 1 .. Count loop
         if Position > Text'Last or else
           Character'Pos (Text (Position)) not in 16#80# .. 16#BF#
         then
            raise Invalid_JSON;
         end if;
         Code := Code * 64 + Character'Pos (Text (Position)) - 16#80#;
         Position := Position + 1;
      end loop;
      if Code < Minimum or else Code > 16#10FFFF# or else
        Code in 16#D800# .. 16#DFFF#
      then
         raise Invalid_JSON;
      end if;
   end Decode;

   procedure Encode (Code : Natural; Text : in out Unbounded_String) is
      procedure Byte (N : Natural) is
      begin
         Append (Text, Character'Val (N));
      end Byte;
   begin
      if Code < 16#80# then
         Byte (Code);
      elsif Code < 16#800# then
         Byte (16#C0# + Code / 64);
         Byte (16#80# + Code mod 64);
      elsif Code < 16#10000# then
         Byte (16#E0# + Code / 4096);
         Byte (16#80# + Code / 64 mod 64);
         Byte (16#80# + Code mod 64);
      else
         Byte (16#F0# + Code / 262144);
         Byte (16#80# + Code / 4096 mod 64);
         Byte (16#80# + Code / 64 mod 64);
         Byte (16#80# + Code mod 64);
      end if;
   end Encode;

   procedure Parse (Source : String; Result : in out Fields;
                    Root : out Value_Kind) is
      type State is (Object_First, Object_Key, Object_Colon, Object_Item,
                     Object_After, Array_First, Array_Item, Array_After);
      -- Iterative parsing bounds nesting memory by input size, without recursion.
      Stack : array (1 .. Source'Length + 1) of State;
      Depth : Natural := 0;
      P : Integer := Source'First;
      Pending : Natural := 0;
      Dummy : Value_Kind;

      procedure Space is
      begin
         while P <= Source'Last and then
           Source (P) in ' ' | ASCII.HT | ASCII.CR | ASCII.LF
         loop
            P := P + 1;
         end loop;
      end Space;

      function Hex4 return Natural is
         N : Natural := 0;
         C : Character;
      begin
         for I in 1 .. 4 loop
            if P > Source'Last then raise Invalid_JSON; end if;
            C := Source (P); P := P + 1; N := N * 16;
            case C is
               when '0' .. '9' => N := N + Character'Pos (C) - 48;
               when 'a' .. 'f' => N := N + Character'Pos (C) - 87;
               when 'A' .. 'F' => N := N + Character'Pos (C) - 55;
               when others => raise Invalid_JSON;
            end case;
         end loop;
         return N;
      end Hex4;

      function Read_String return Unbounded_String is
         Text : Unbounded_String;
         Code, Low : Natural;
         Start : Integer;
         C : Character;
      begin
         if P > Source'Last or else Source (P) /= '"' then
            raise Invalid_JSON;
         end if;
         P := P + 1;
         while P <= Source'Last loop
            C := Source (P); P := P + 1;
            if C = '"' then return Text;
            elsif C = '\' then
               if P > Source'Last then raise Invalid_JSON; end if;
               C := Source (P); P := P + 1;
               case C is
                  when '"' | '\' | '/' => Append (Text, C);
                  when 'b' => Append (Text, ASCII.BS);
                  when 'f' => Append (Text, ASCII.FF);
                  when 'n' => Append (Text, ASCII.LF);
                  when 'r' => Append (Text, ASCII.CR);
                  when 't' => Append (Text, ASCII.HT);
                  when 'u' =>
                     Code := Hex4;
                     if Code in 16#D800# .. 16#DBFF# then
                        if P + 1 > Source'Last or else
                          Source (P .. P + 1) /= "\u"
                        then
                           raise Invalid_JSON;
                        end if;
                        P := P + 2;
                        Low := Hex4;
                        if Low not in 16#DC00# .. 16#DFFF# then
                           raise Invalid_JSON;
                        end if;
                        Code := 16#10000# +
                          (Code - 16#D800#) * 1024 + Low - 16#DC00#;
                     elsif Code in 16#DC00# .. 16#DFFF# then
                        raise Invalid_JSON;
                     end if;
                     Encode (Code, Text);
                  when others => raise Invalid_JSON;
               end case;
            elsif Character'Pos (C) < 32 then
               raise Invalid_JSON;
            elsif Character'Pos (C) >= 128 then
               P := P - 1; Start := P;
               Decode (Source, P, Code);
               Append (Text, Source (Start .. P - 1));
            else
               -- Copy an ordinary ASCII run at once. Escapes and UTF-8 still
               -- take the validating paths above; no parsed data is retained.
               Start := P - 1;
               while P <= Source'Last and then Source (P) not in '"' | '\'
                 and then Character'Pos (Source (P)) in 32 .. 127
               loop
                  P := P + 1;
               end loop;
               Append (Text, Source (Start .. P - 1));
            end if;
         end loop;
         raise Invalid_JSON;
      end Read_String;

      procedure Literal (S : String) is
      begin
         if P + S'Length - 1 > Source'Last or else
           Source (P .. P + S'Length - 1) /= S
         then
            raise Invalid_JSON;
         end if;
         P := P + S'Length;
      end Literal;

      procedure Take_Value (Slot : Natural; Kind : out Value_Kind) is
         Text : Unbounded_String;
         Start : Integer;
         procedure Consume_Digits is
            Before : constant Integer := P;
         begin
            while P <= Source'Last and then Source (P) in '0' .. '9' loop
               P := P + 1;
            end loop;
            if P = Before then raise Invalid_JSON; end if;
         end Consume_Digits;
      begin
         Space;
         if P > Source'Last then raise Invalid_JSON; end if;
         case Source (P) is
            when '{' =>
               Kind := Object_Value; Depth := Depth + 1;
               Stack (Depth) := Object_First; P := P + 1;
            when '[' =>
               Kind := Array_Value; Depth := Depth + 1;
               Stack (Depth) := Array_First; P := P + 1;
            when '"' => Kind := String_Value; Text := Read_String;
            when 't' => Kind := Boolean_Value; Literal ("true");
            when 'f' => Kind := Boolean_Value; Literal ("false");
            when 'n' => Kind := Null_Value; Literal ("null");
            when '-' | '0' .. '9' =>
               Kind := Number_Value; Start := P;
               if Source (P) = '-' then P := P + 1; end if;
               if P > Source'Last then raise Invalid_JSON; end if;
               if Source (P) = '0' then P := P + 1;
               else Consume_Digits;
               end if;
               if P <= Source'Last and then Source (P) = '.' then
                  P := P + 1; Consume_Digits;
               end if;
               if P <= Source'Last and then Source (P) in 'e' | 'E' then
                  P := P + 1;
                  if P <= Source'Last and then Source (P) in '+' | '-' then
                     P := P + 1;
                  end if;
                  Consume_Digits;
               end if;
               Text := To_Unbounded_String (Source (Start .. P - 1));
            when others => raise Invalid_JSON;
         end case;
         if Slot /= 0 then
            Result (Slot).Kind := Kind;
            Result (Slot).Text := Text;
         end if;
      end Take_Value;

      procedure Key is
         Name : constant Unbounded_String := Read_String;
      begin
         if Depth = 1 then
            Pending := 0;
            for I in Result'Range loop
               if Result (I).Name = Name then Pending := I; end if;
            end loop;
         end if;
         Stack (Depth) := Object_Colon;
      end Key;
   begin
      for F of Result loop
         F.Kind := Missing; F.Text := Null_Unbounded_String;
      end loop;
      Take_Value (0, Root);
      while Depth > 0 loop
         Space;
         if P > Source'Last then raise Invalid_JSON; end if;
         case Stack (Depth) is
            when Object_First | Object_Key =>
               if Stack (Depth) = Object_First and then Source (P) = '}' then
                  Depth := Depth - 1; P := P + 1;
               else
                  Key;
               end if;
            when Object_Colon =>
               if Source (P) /= ':' then raise Invalid_JSON; end if;
               P := P + 1; Stack (Depth) := Object_Item;
            when Object_Item =>
               Stack (Depth) := Object_After;
               if Depth = 1 then Take_Value (Pending, Dummy);
               else Take_Value (0, Dummy);
               end if;
            when Object_After =>
               if Source (P) = '}' then
                  Depth := Depth - 1; P := P + 1;
               elsif Source (P) = ',' then
                  Stack (Depth) := Object_Key; P := P + 1;
               else raise Invalid_JSON;
               end if;
            when Array_First | Array_Item =>
               if Stack (Depth) = Array_First and then Source (P) = ']' then
                  Depth := Depth - 1; P := P + 1;
               else
                  Stack (Depth) := Array_After; Take_Value (0, Dummy);
               end if;
            when Array_After =>
               if Source (P) = ']' then
                  Depth := Depth - 1; P := P + 1;
               elsif Source (P) = ',' then
                  Stack (Depth) := Array_Item; P := P + 1;
               else raise Invalid_JSON;
               end if;
         end case;
      end loop;
      Space;
      if P <= Source'Last then raise Invalid_JSON; end if;
   end Parse;

   function Quote (Text : String) return String is
      Extra : Natural := 0;
      Hex : constant String := "0123456789abcdef";
   begin
      -- Most database strings need no escaping. Size the output first so even
      -- the escaped path avoids a managed-string append for every input byte.
      for C of Text loop
         case C is
            when '"' | '\' | ASCII.BS | ASCII.FF | ASCII.LF | ASCII.CR | ASCII.HT =>
               Extra := Extra + 1;
            when others =>
               if Character'Pos (C) < 32 then Extra := Extra + 5; end if;
         end case;
      end loop;
      if Extra = 0 then return '"' & Text & '"'; end if;
      declare
         Result : String (1 .. Text'Length + Extra + 2);
         P : Positive := 2;
         procedure Put (Value : String) is
         begin
            Result (P .. P + Value'Length - 1) := Value;
            P := P + Value'Length;
         end Put;
      begin
         Result (1) := '"';
         for C of Text loop
            case C is
               when '"' => Put ("\""");
               when '\' => Put ("\\");
               when ASCII.BS => Put ("\b");
               when ASCII.FF => Put ("\f");
               when ASCII.LF => Put ("\n");
               when ASCII.CR => Put ("\r");
               when ASCII.HT => Put ("\t");
               when others =>
                  if Character'Pos (C) < 32 then
                     Put ("\u00" & Hex (Character'Pos (C) / 16 + 1) &
                       Hex (Character'Pos (C) mod 16 + 1));
                  else
                     Result (P) := C; P := P + 1;
                  end if;
            end case;
         end loop;
         Result (P) := '"';
         return Result;
      end;
   end Quote;

   function Is_Space (Code : Natural) return Boolean is
     (Code in 9 .. 13 | 32 | 16#85# | 16#A0# | 16#1680# |
        16#2000# .. 16#200A# | 16#2028# | 16#2029# | 16#202F# |
        16#205F# | 16#3000# | 16#FEFF#);

   function Trim (Text : String) return String is
      P : Integer := Text'First;
      First : Integer := Text'Last + 1;
      Last : Integer := Text'First - 1;
      Start : Integer;
      Code : Natural;
   begin
      while P <= Text'Last loop
         Start := P;
         Decode (Text, P, Code);
         if not Is_Space (Code) then
            First := Integer'Min (First, Start);
            Last := P - 1;
         end if;
      end loop;
      return Text (First .. Last);
   end Trim;

   function Character_Count (Text : String) return Natural is
      P : Integer := Text'First;
      Count : Natural := 0;
      Code : Natural;
   begin
      while P <= Text'Last loop
         Decode (Text, P, Code);
         Count := Count + 1;
      end loop;
      return Count;
   end Character_Count;

   function Positive_ID (Text : String) return Long_Long_Integer is
      N : Long_Long_Integer := 0;
   begin
      if Text'Length = 0 then return 0; end if;
      for C of Text loop
         if C not in '0' .. '9' then return 0; end if;
         N := N * 10 + Long_Long_Integer (Character'Pos (C) - 48);
      end loop;
      return N;
   exception
      when Constraint_Error => return 0;
   end Positive_ID;

   function Number (Value : Long_Long_Integer) return String is
     (Ada.Strings.Fixed.Trim (Long_Long_Integer'Image (Value), Ada.Strings.Both));
end JSON;
