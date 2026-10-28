with Ada.Characters.Handling;
with Ada.Strings.Fixed;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with JSON;

package body HTTP is
   use Ada.Strings.Fixed;
   CRLF : constant String := ASCII.CR & ASCII.LF;

   function OWS (S : String) return String is
      First : Integer := S'First;
      Last : Integer := S'Last;
   begin
      while First <= Last and then S (First) in ' ' | ASCII.HT loop
         First := First + 1;
      end loop;
      while Last >= First and then S (Last) in ' ' | ASCII.HT loop
         Last := Last - 1;
      end loop;
      return S (First .. Last);
   end OWS;

   function Token (S : String) return Boolean is
   begin
      if S'Length = 0 then return False; end if;
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

   procedure Parse (Input : String; Item : out Request;
                    Status : out Parse_Status; Consumed : out Natural) is
      Header_End : constant Natural := Index (Input, CRLF & CRLF);
      Line_End, A, B, P, E, Colon : Natural;
      Content_Length : Natural := 0;
      Seen_Length, Chunked, Seen_Auth, Seen_Host : Boolean := False;
      Seen_TE, HTTP_11 : Boolean := False;
      Content : Unbounded_String;

      procedure Parse_Chunks (Start : Natural) is
         Pos : Natural := Start;
         Size, Digits_Count, Value : Natural;
         End_Line : Natural;
      begin
         loop
            if Pos > Input'Last then return; end if;
            End_Line := Index (Input, CRLF, Pos);
            if End_Line = 0 then return; end if;
            Size := 0; Digits_Count := 0;
            for I in Pos .. End_Line - 1 loop
               exit when Input (I) = ';';
               case Input (I) is
                  when '0' .. '9' => Value := Character'Pos (Input (I)) - 48;
                  when 'a' .. 'f' => Value := Character'Pos (Input (I)) - 87;
                  when 'A' .. 'F' => Value := Character'Pos (Input (I)) - 55;
                  when others => Status := Bad_Request; return;
               end case;
               Digits_Count := Digits_Count + 1;
               Size := Size * 16 + Value;
               if Size > Max_Body then Status := Too_Large; return; end if;
            end loop;
            if Digits_Count = 0 then Status := Bad_Request; return; end if;
            Pos := End_Line + 2;
            if Size = 0 then
               -- Consume trailers without allowing them to redefine framing/auth.
               loop
                  if Pos > Input'Last then return; end if;
                  End_Line := Index (Input, CRLF, Pos);
                  if End_Line = 0 then return; end if;
                  if End_Line = Pos then
                     Consumed := End_Line + 1 - Input'First + 1;
                     Item.Payload := Content; Status := Complete; return;
                  end if;
                  if Index (Input (Pos .. End_Line - 1), ":") = 0 then
                     Status := Bad_Request; return;
                  end if;
                  Pos := End_Line + 2;
               end loop;
            end if;
            if Length (Content) + Size > Max_Body then
               Status := Too_Large; return;
            end if;
            if Pos + Size + 1 > Input'Last then return; end if;
            if Input (Pos + Size .. Pos + Size + 1) /= CRLF then
               Status := Bad_Request; return;
            end if;
            Append (Content, Input (Pos .. Pos + Size - 1));
            Pos := Pos + Size + 2;
         end loop;
      end Parse_Chunks;
   begin
      Item := (others => <>); Status := Incomplete; Consumed := 0;
      if Header_End = 0 then
         if Input'Length >= Max_Header then Status := Too_Large; end if;
         return;
      end if;
      if Header_End + 3 - Input'First + 1 > Max_Header then
         Status := Too_Large; return;
      end if;
      Status := Bad_Request;
      Line_End := Index (Input, CRLF);
      A := Index (Input (Input'First .. Line_End - 1), " ");
      if A = 0 then return; end if;
      B := Index (Input (A + 1 .. Line_End - 1), " ");
      if B = 0 or else B = A + 1 then return; end if;
      if not Token (Input (Input'First .. A - 1)) then return; end if;
      Item.Method := To_Unbounded_String (Input (Input'First .. A - 1));
      if Input (B + 1 .. Line_End - 1) = "HTTP/1.1" then
         HTTP_11 := True;
      elsif Input (B + 1 .. Line_End - 1) = "HTTP/1.0" then
         Item.Close := True;
      else return;
      end if;
      if Input (A + 1) /= '/' then return; end if;
      for C of Input (A + 1 .. B - 1) loop
         if Character'Pos (C) <= 32 or else C = ASCII.DEL then return; end if;
      end loop;
      E := Index (Input (A + 1 .. B - 1), "?");
      if E = 0 then E := B; end if;
      Item.Path := To_Unbounded_String (Input (A + 1 .. E - 1));
      P := Line_End + 2;
      while P < Header_End loop
         E := Index (Input, CRLF, P);
         Colon := Index (Input (P .. E - 1), ":");
         if Colon = 0 or else not Token (Input (P .. Colon - 1)) then return; end if;
         declare
            Name : constant String := Ada.Characters.Handling.To_Lower (Input (P .. Colon - 1));
            Value : constant String := OWS (Input (Colon + 1 .. E - 1));
            Lower : constant String := Ada.Characters.Handling.To_Lower (Value);
         begin
            for C of Value loop
               if (Character'Pos (C) < 32 and then C /= ASCII.HT)
                 or else C = ASCII.DEL
               then
                  return;
               end if;
            end loop;
            if Name = "content-length" then
               if Seen_Length or else Value'Length = 0 then return; end if;
               Seen_Length := True;
               for C of Value loop
                  if C not in '0' .. '9' then return; end if;
                  Content_Length := Content_Length * 10 + Character'Pos (C) - 48;
                  if Content_Length > Max_Body then Status := Too_Large; return; end if;
               end loop;
            elsif Name = "transfer-encoding" then
               if Seen_TE or else Lower /= "chunked" then return; end if;
               Seen_TE := True; Chunked := True;
            elsif Name = "authorization" then
               if Seen_Auth then return; end if;
               Seen_Auth := True; Item.Authorization := To_Unbounded_String (Value);
            elsif Name = "host" then
               if Seen_Host or else Value'Length = 0 then return; end if;
               Seen_Host := True;
            elsif Name = "connection" then
               if Index (Lower, "close") > 0 then Item.Close := True;
               elsif not HTTP_11 and then Index (Lower, "keep-alive") > 0 then
                  Item.Close := False;
               end if;
            elsif Name = "expect" then
               if Lower /= "100-continue" then return; end if;
               Item.Expect_Continue := True;
            end if;
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
         Consumed := Header_End + 3 + Content_Length - Input'First + 1;
         if Input'Length < Consumed then Consumed := 0; return; end if;
         Item.Payload := To_Unbounded_String
           (Input (Header_End + 4 .. Header_End + 3 + Content_Length));
         Status := Complete;
      end if;
   exception
      when Constraint_Error =>
         Status := Bad_Request; Consumed := 0;
   end Parse;

   function Serialize (Reply : Application.Response; Close : Boolean) return String is
      Reason : constant String :=
        (case Reply.Status is
           when 200 => "OK", when 201 => "Created",
           when 400 => "Bad Request", when 401 => "Unauthorized",
           when 404 => "Not Found", when 413 => "Payload Too Large",
           when 503 => "Service Unavailable", when others => "Internal Server Error");
   begin
      return "HTTP/1.1 " & JSON.Number (Long_Long_Integer (Reply.Status)) &
        " " & Reason & CRLF & "Content-Type: application/json" & CRLF &
        "Content-Length: " & JSON.Number (Long_Long_Integer (Length (Reply.Content))) &
        CRLF & (if Close then "Connection: close" else "Connection: keep-alive") &
        CRLF & (if Close then "" else "Keep-Alive: timeout=75" & CRLF) &
        CRLF & To_String (Reply.Content);
   end Serialize;
end HTTP;
