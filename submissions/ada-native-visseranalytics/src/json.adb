package body JSON is
   -- Validate one UTF-8 sequence at Position and return its code point.
   procedure Next_Code (Text : String; Position : in out Integer; Code : out Natural) is
      First, Count, Minimum : Natural;
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
   end Next_Code;

   function Hex_Value (C : Character) return Natural is
     (case C is
        when '0' .. '9' => Character'Pos (C) - 48,
        when 'a' .. 'f' => Character'Pos (C) - 87,
        when 'A' .. 'F' => Character'Pos (C) - 55,
        when others => raise Invalid_JSON);

   function Hex4 (Text : String; At_Index : Integer) return Natural is
      N : Natural := 0;
   begin
      if At_Index + 3 > Text'Last then
         raise Invalid_JSON;
      end if;
      for I in At_Index .. At_Index + 3 loop
         N := N * 16 + Hex_Value (Text (I));
      end loop;
      return N;
   end Hex4;

   procedure Parse (Source : String; Names : Name_Array;
                    Result : out Field_Array; Root : out Value_Kind) is
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

      procedure Read_String (First : out Positive; Last : out Natural;
                             Escaped : out Boolean) is
         Code, Low : Natural;
         C : Character;
      begin
         if P > Source'Last or else Source (P) /= '"' then
            raise Invalid_JSON;
         end if;
         P := P + 1;
         First := P;
         Escaped := False;
         while P <= Source'Last loop
            C := Source (P);
            if C = '"' then
               Last := P - 1;
               P := P + 1;
               return;
            elsif C = '\' then
               Escaped := True;
               P := P + 1;
               if P > Source'Last then raise Invalid_JSON; end if;
               C := Source (P);
               P := P + 1;
               case C is
                  when '"' | '\' | '/' | 'b' | 'f' | 'n' | 'r' | 't' => null;
                  when 'u' =>
                     Code := Hex4 (Source, P);
                     P := P + 4;
                     if Code in 16#D800# .. 16#DBFF# then
                        if P + 1 > Source'Last or else Source (P) /= '\'
                          or else Source (P + 1) /= 'u'
                        then
                           raise Invalid_JSON;
                        end if;
                        Low := Hex4 (Source, P + 2);
                        P := P + 6;
                        if Low not in 16#DC00# .. 16#DFFF# then
                           raise Invalid_JSON;
                        end if;
                     elsif Code in 16#DC00# .. 16#DFFF# then
                        raise Invalid_JSON;
                     end if;
                  when others => raise Invalid_JSON;
               end case;
            elsif Character'Pos (C) < 32 then
               raise Invalid_JSON;
            elsif Character'Pos (C) >= 128 then
               Next_Code (Source, P, Code);
            else
               -- An ordinary ASCII run; escapes and UTF-8 take the paths above.
               P := P + 1;
               while P <= Source'Last and then Source (P) not in '"' | '\'
                 and then Character'Pos (Source (P)) in 32 .. 127
               loop
                  P := P + 1;
               end loop;
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
         Item : Field;
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
            when '"' =>
               Kind := String_Value;
               Read_String (Item.First, Item.Last, Item.Escaped);
            when 't' => Kind := Boolean_Value; Literal ("true");
            when 'f' => Kind := Boolean_Value; Literal ("false");
            when 'n' => Kind := Null_Value; Literal ("null");
            when '-' | '0' .. '9' =>
               Kind := Number_Value;
               Item.First := P;
               if Source (P) = '-' then P := P + 1; end if;
               if P > Source'Last then raise Invalid_JSON; end if;
               if Source (P) = '0' then
                  P := P + 1;
               else
                  Consume_Digits;
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
               Item.Last := P - 1;
            when others => raise Invalid_JSON;
         end case;
         if Slot /= 0 then
            Item.Kind := Kind;
            Result (Slot) := Item;
         end if;
      end Take_Value;

      procedure Key is
         First : Positive;
         Last : Natural;
         Escaped : Boolean;
      begin
         Read_String (First, Last, Escaped);
         if Depth = 1 then
            Pending := 0;
            for I in Names'Range loop
               if (if Escaped then Equals (Source, (String_Value, First, Last, True), Names (I).all)
                   else Source (First .. Last) = Names (I).all)
               then
                  Pending := I;
               end if;
            end loop;
         end if;
         Stack (Depth) := Object_Colon;
      end Key;
   begin
      Result := [others => (others => <>)];
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
               if Depth = 1 then
                  Take_Value (Pending, Dummy);
               else
                  Take_Value (0, Dummy);
               end if;
            when Object_After =>
               if Source (P) = '}' then
                  Depth := Depth - 1; P := P + 1;
               elsif Source (P) = ',' then
                  Stack (Depth) := Object_Key; P := P + 1;
               else
                  raise Invalid_JSON;
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
               else
                  raise Invalid_JSON;
               end if;
         end case;
      end loop;
      Space;
      if P <= Source'Last then raise Invalid_JSON; end if;
   end Parse;

   procedure Decode (Source : String; F : Field; Target : in out String;
                     Length : out Natural) is
      I : Integer := F.First;
      J : Integer;
      N : Natural := Target'First - 1;
      Code, Low : Natural;

      procedure Byte (Value : Natural) is
      begin
         N := N + 1;
         Target (N) := Character'Val (Value);
      end Byte;
   begin
      while I <= F.Last loop
         if Source (I) /= '\' then
            J := I;
            while J <= F.Last and then Source (J) /= '\' loop
               J := J + 1;
            end loop;
            Target (N + 1 .. N + J - I) := Source (I .. J - 1);
            N := N + J - I;
            I := J;
         else
            case Source (I + 1) is
               when '"' | '\' | '/' => Byte (Character'Pos (Source (I + 1)));
               when 'b' => Byte (8);
               when 'f' => Byte (12);
               when 'n' => Byte (10);
               when 'r' => Byte (13);
               when 't' => Byte (9);
               when 'u' =>
                  Code := Hex4 (Source, I + 2);
                  if Code in 16#D800# .. 16#DBFF# then
                     Low := Hex4 (Source, I + 8);
                     Code := 16#10000# + (Code - 16#D800#) * 1024 + Low - 16#DC00#;
                     I := I + 6;
                  end if;
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
                  I := I + 4;
               when others => raise Invalid_JSON;
            end case;
            I := I + 2;
         end if;
      end loop;
      Length := N - Target'First + 1;
   end Decode;

   function Equals (Source : String; F : Field; Text : String) return Boolean is
   begin
      if not F.Escaped then
         return Source (F.First .. F.Last) = Text;
      end if;
      declare
         Buffer : String (1 .. Natural'Max (F.Last - F.First + 1, 0));
         N : Natural;
      begin
         Decode (Source, F, Buffer, N);
         return Buffer (1 .. N) = Text;
      end;
   end Equals;

   function Is_Space (Code : Natural) return Boolean is
     (Code in 9 .. 13 | 32 | 16#85# | 16#A0# | 16#1680# |
        16#2000# .. 16#200A# | 16#2028# | 16#2029# | 16#202F# |
        16#205F# | 16#3000# | 16#FEFF#);

   procedure Trim (Text : String; First : out Positive; Last : out Natural) is
      P : Integer := Text'First;
      Start : Integer;
      Code : Natural;
   begin
      First := Text'Last + 1;
      Last := Text'First - 1;
      while P <= Text'Last loop
         Start := P;
         Next_Code (Text, P, Code);
         if not Is_Space (Code) then
            First := Integer'Min (First, Start);
            Last := P - 1;
         end if;
      end loop;
   end Trim;

   function Character_Count (Text : String) return Natural is
      P : Integer := Text'First;
      Count : Natural := 0;
      Code : Natural;
   begin
      while P <= Text'Last loop
         Next_Code (Text, P, Code);
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
end JSON;
