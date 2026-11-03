with Interfaces.C;
with System;
with System.Storage_Elements;

package body HTTP is
   use type System.Address;
   CR : constant Character := ASCII.CR;
   LF : constant Character := ASCII.LF;

   function Lower (C : Character) return Character is
     (if C in 'A' .. 'Z' then Character'Val (Character'Pos (C) + 32) else C)
     with Inline;

   -- Case-insensitive comparison against an all-lowercase ASCII Name.
   function Same (Text : String; Name : String) return Boolean is
   begin
      if Text'Length /= Name'Length then
         return False;
      end if;
      for I in 0 .. Text'Length - 1 loop
         if Lower (Text (Text'First + I)) /= Name (Name'First + I) then
            return False;
         end if;
      end loop;
      return True;
   end Same;

   function Contains (Text : String; Name : String) return Boolean is
   begin
      for Start in Text'First .. Text'Last - Name'Length + 1 loop
         if Same (Text (Start .. Start + Name'Length - 1), Name) then
            return True;
         end if;
      end loop;
      return False;
   end Contains;

   function Memchr (Source : System.Address; Item : Interfaces.C.int;
                    Count : Interfaces.C.size_t) return System.Address
     with Import, Convention => C, External_Name => "memchr";

   -- First CR LF at or after From (0 if none). libc's memchr skips ahead
   -- to each CR with vector instructions.
   function Find_CRLF (Input : String; From : Positive) return Natural is
      use System.Storage_Elements;
      I : Positive := From;
      Found : System.Address;
   begin
      while I < Input'Last loop
         Found := Memchr (Input (I)'Address, Character'Pos (CR),
                          Interfaces.C.size_t (Input'Last - I));
         exit when Found = System.Null_Address;
         I := I + Natural (Found - Input (I)'Address);
         if Input (I + 1) = LF then
            return I;
         end if;
         I := I + 1;
      end loop;
      return 0;
   end Find_CRLF;

   function Find_Char (Input : String; Item : Character) return Natural is
   begin
      for I in Input'Range loop
         if Input (I) = Item then
            return I;
         end if;
      end loop;
      return 0;
   end Find_Char;

   function Token (S : String) return Boolean is
   begin
      if S'Length = 0 then
         return False;
      end if;
      for C of S loop
         if C not in 'a' .. 'z' | 'A' .. 'Z' | '0' .. '9' |
           '!' | '#' | '$' | '%' | '&' | ''' | '*' | '+' | '-' |
           '.' | '^' | '_' | '`' | '|' | '~'
         then
            return False;
         end if;
      end loop;
      return True;
   end Token;

   procedure Parse (Input : String; Item : out Request; Status : out Parse_Status) is
      Header_End : Natural := 0;
      Line_End, A, B, P, E, Colon, First, Last : Natural;
      Content_Length : Natural := 0;
      Seen_Length, Chunked, Seen_Auth, Seen_Host, Seen_TE, HTTP_11 : Boolean := False;

      procedure Parse_Chunks (Start : Positive) is
         Pos : Positive := Start;
         Size, Digit_Count, Value : Natural;
         End_Line : Natural;
      begin
         Chunk_Length := 0;
         loop
            if Pos > Input'Last then return; end if;
            End_Line := Find_CRLF (Input, Pos);
            if End_Line = 0 then return; end if;
            Size := 0;
            Digit_Count := 0;
            for I in Pos .. End_Line - 1 loop
               exit when Input (I) = ';';
               case Input (I) is
                  when '0' .. '9' => Value := Character'Pos (Input (I)) - 48;
                  when 'a' .. 'f' => Value := Character'Pos (Input (I)) - 87;
                  when 'A' .. 'F' => Value := Character'Pos (Input (I)) - 55;
                  when others => Status := Bad_Request; return;
               end case;
               Digit_Count := Digit_Count + 1;
               Size := Size * 16 + Value;
               if Size > Max_Body then
                  Status := Too_Large; return;
               end if;
            end loop;
            if Digit_Count = 0 then
               Status := Bad_Request; return;
            end if;
            Pos := End_Line + 2;
            if Size = 0 then
               -- Consume trailers without letting them redefine framing or auth.
               loop
                  if Pos > Input'Last then return; end if;
                  End_Line := Find_CRLF (Input, Pos);
                  if End_Line = 0 then return; end if;
                  if End_Line = Pos then
                     Item.Consumed := End_Line + 2 - Input'First;
                     Item.Chunked := True;
                     Status := Complete;
                     return;
                  end if;
                  if Find_Char (Input (Pos .. End_Line - 1), ':') = 0 then
                     Status := Bad_Request; return;
                  end if;
                  Pos := End_Line + 2;
               end loop;
            end if;
            if Chunk_Length + Size > Max_Body then
               Status := Too_Large; return;
            end if;
            if Pos + Size + 1 > Input'Last then return; end if;
            if Input (Pos + Size) /= CR or else Input (Pos + Size + 1) /= LF then
               Status := Bad_Request; return;
            end if;
            Chunk_Data (Chunk_Length + 1 .. Chunk_Length + Size) := Input (Pos .. Pos + Size - 1);
            Chunk_Length := Chunk_Length + Size;
            Pos := Pos + Size + 2;
         end loop;
      end Parse_Chunks;
   begin
      Item := (others => <>);
      Status := Incomplete;
      declare
         P : Natural := Find_CRLF (Input, Input'First);
      begin
         while P /= 0 and then P + 3 <= Input'Last loop
            if Input (P + 2) = CR and then Input (P + 3) = LF then
               Header_End := P;
               exit;
            end if;
            P := Find_CRLF (Input, P + 2);
         end loop;
      end;
      if Header_End = 0 then
         if Input'Length >= Max_Header then Status := Too_Large; end if;
         return;
      end if;
      if Header_End + 4 - Input'First > Max_Header then
         Status := Too_Large; return;
      end if;
      Status := Bad_Request;
      Line_End := Find_CRLF (Input, Input'First);
      A := Find_Char (Input (Input'First .. Line_End - 1), ' ');
      if A = 0 then return; end if;
      B := Find_Char (Input (A + 1 .. Line_End - 1), ' ');
      if B = 0 or else B = A + 1 then return; end if;
      if not Token (Input (Input'First .. A - 1)) then return; end if;
      declare
         Method : String renames Input (Input'First .. A - 1);
      begin
         if Method = "GET" then
            Item.Method := Get;
         elsif Method = "POST" then
            Item.Method := Post;
         end if;
      end;
      declare
         Version : String renames Input (B + 1 .. Line_End - 1);
      begin
         if Version = "HTTP/1.1" then
            HTTP_11 := True;
         elsif Version = "HTTP/1.0" then
            Item.Close := True;
         else
            return;
         end if;
      end;
      if Input (A + 1) /= '/' then return; end if;
      for C of Input (A + 1 .. B - 1) loop
         if Character'Pos (C) <= 32 or else C = ASCII.DEL then return; end if;
      end loop;
      E := Find_Char (Input (A + 1 .. B - 1), '?');
      if E = 0 then E := B; end if;
      Item.Path_First := A + 1;
      Item.Path_Last := E - 1;
      P := Line_End + 2;
      while P < Header_End loop
         E := Find_CRLF (Input, P);
         Colon := Find_Char (Input (P .. E - 1), ':');
         if Colon = 0 or else not Token (Input (P .. Colon - 1)) then return; end if;
         -- Value without optional surrounding whitespace.
         First := Colon + 1;
         Last := E - 1;
         while First <= Last and then Input (First) in ' ' | ASCII.HT loop
            First := First + 1;
         end loop;
         while Last >= First and then Input (Last) in ' ' | ASCII.HT loop
            Last := Last - 1;
         end loop;
         for I in First .. Last loop
            if (Character'Pos (Input (I)) < 32 and then Input (I) /= ASCII.HT)
              or else Input (I) = ASCII.DEL
            then
               return;
            end if;
         end loop;
         declare
            Name : String renames Input (P .. Colon - 1);
            Value : String renames Input (First .. Last);
         begin
            case Name'Length is
               when 4 =>
                  if Same (Name, "host") then
                     if Seen_Host or else Value'Length = 0 then return; end if;
                     Seen_Host := True;
                  end if;
               when 6 =>
                  if Same (Name, "expect") then
                     if not Same (Value, "100-continue") then return; end if;
                     Item.Expect_Continue := True;
                  end if;
               when 10 =>
                  if Same (Name, "connection") then
                     if Contains (Value, "close") then
                        Item.Close := True;
                     elsif not HTTP_11 and then Contains (Value, "keep-alive") then
                        Item.Close := False;
                     end if;
                  end if;
               when 13 =>
                  if Same (Name, "authorization") then
                     if Seen_Auth then return; end if;
                     Seen_Auth := True;
                     Item.Has_Auth := True;
                     Item.Auth_First := First;
                     Item.Auth_Last := Last;
                  end if;
               when 14 =>
                  if Same (Name, "content-length") then
                     if Seen_Length or else Value'Length = 0 then return; end if;
                     Seen_Length := True;
                     for C of Value loop
                        if C not in '0' .. '9' then return; end if;
                        Content_Length := Content_Length * 10 + Character'Pos (C) - 48;
                        if Content_Length > Max_Body then
                           Status := Too_Large; return;
                        end if;
                     end loop;
                  end if;
               when 17 =>
                  if Same (Name, "transfer-encoding") then
                     if Seen_TE or else not Same (Value, "chunked") then return; end if;
                     Seen_TE := True;
                     Chunked := True;
                  end if;
               when others =>
                  null;
            end case;
         end;
         P := E + 2;
      end loop;
      if (HTTP_11 and then not Seen_Host) or else (Chunked and then Seen_Length) then
         return;
      end if;
      Status := Incomplete;
      if Chunked then
         Parse_Chunks (Header_End + 4);
      else
         if Input'Length < Header_End + 4 + Content_Length - Input'First then
            return;
         end if;
         Item.Consumed := Header_End + 4 + Content_Length - Input'First;
         Item.Body_First := Header_End + 4;
         Item.Body_Last := Header_End + 3 + Content_Length;
         Status := Complete;
      end if;
   exception
      when Constraint_Error =>
         Status := Bad_Request;
         Item.Consumed := 0;
   end Parse;
end HTTP;
