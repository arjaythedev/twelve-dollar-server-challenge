with Interfaces; use Interfaces;
with Interfaces.C;
with Pool;
with Sys;

package body Output is
   Head_Room : constant := 256;
   Body_Start : constant := Head_Room + 1;
   -- Larger than any response: 20 posts of 500 fully escaped characters or
   -- a token username from a 16 KiB header each stay far below this.
   Buffer : aliased String (1 .. Head_Room + Pool.Largest);
   Pos : Positive := Body_Start;
   CRLF : constant String := ASCII.CR & ASCII.LF;

   procedure Start is
   begin
      Pos := Body_Start;
   end Start;

   -- One explicit capacity check per write; the copies below are then
   -- known to stay inside Buffer, so their index checks are suppressed.
   procedure Reserve (Bytes : Natural) is
   begin
      if Bytes > Buffer'Last + 1 - Pos then
         raise Constraint_Error with "response too large";
      end if;
   end Reserve;
   pragma Inline (Reserve);

   procedure Put (Text : String) is
      pragma Suppress (All_Checks);
   begin
      Reserve (Text'Length);
      Buffer (Pos .. Pos + Text'Length - 1) := Text;
      Pos := Pos + Text'Length;
   end Put;

   procedure Put (Item : Character) is
      pragma Suppress (All_Checks);
   begin
      Reserve (1);
      Buffer (Pos) := Item;
      Pos := Pos + 1;
   end Put;

   Pairs : constant String (1 .. 200) :=
     "00010203040506070809101112131415161718192021222324252627282930313233343536373839" &
     "40414243444546474849505152535455565758596061626364656667686970717273747576777879" &
     "8081828384858687888990919293949596979899";

   procedure Put_Int (Value : Long_Long_Integer) is
      pragma Suppress (All_Checks);
      Text : String (1 .. 20);
      N : Natural := Text'Last;
      V : Unsigned_64;
      R : Natural;
   begin
      Reserve (20);
      if Value in 0 .. 9 then
         Buffer (Pos) := Character'Val (48 + Natural (Value));
         Pos := Pos + 1;
         return;
      elsif Value < 0 then
         V := Unsigned_64 (-(Value + 1)) + 1;
      else
         V := Unsigned_64 (Value);
      end if;
      while V >= 100 loop
         R := Natural (V mod 100);
         V := V / 100;
         Text (N - 1 .. N) := Pairs (2 * R + 1 .. 2 * R + 2);
         N := N - 2;
      end loop;
      if V >= 10 then
         R := Natural (V);
         Text (N - 1 .. N) := Pairs (2 * R + 1 .. 2 * R + 2);
         N := N - 2;
      else
         Text (N) := Character'Val (48 + Natural (V));
         N := N - 1;
      end if;
      if Value < 0 then
         Text (N) := '-';
         N := N - 1;
      end if;
      Buffer (Pos .. Pos + Text'Last - N - 1) := Text (N + 1 .. Text'Last);
      Pos := Pos + Text'Last - N;
   end Put_Int;

   Hex : constant String := "0123456789abcdef";

   -- Bytes needing an escape. Branch-free so the compiler vectorizes it.
   function Escapes (Text : String) return Natural is
      pragma Suppress (All_Checks);
      Count : Natural := 0;
   begin
      for C of Text loop
         Count := Count + Boolean'Pos (C < ' ' or C = '"' or C = '\');
      end loop;
      return Count;
   end Escapes;

   procedure Put_Quoted (Address : System.Address; Length : Natural) is
      pragma Suppress (All_Checks);
      Text : String (1 .. Length) with Import, Address => Address;
      Run : Natural := 1;
      C : Character;

      procedure Escape (Code : String) is
      begin
         Buffer (Pos .. Pos + Code'Length - 1) := Code;
         Pos := Pos + Code'Length;
      end Escape;
   begin
      -- Worst case: every byte becomes a six-byte \u00XX escape.
      Reserve (6 * Length + 2);
      Buffer (Pos) := '"';
      Pos := Pos + 1;
      if Escapes (Text) = 0 then
         -- The usual case: the text goes out unchanged.
         Buffer (Pos .. Pos + Length - 1) := Text;
         Pos := Pos + Length;
      else
         for I in 1 .. Length loop
            C := Text (I);
            if C = '"' or else C = '\' or else C < ' ' then
               if I > Run then
                  Buffer (Pos .. Pos + I - 1 - Run) := Text (Run .. I - 1);
                  Pos := Pos + I - Run;
               end if;
               case C is
                  when '"' => Escape ("\""");
                  when '\' => Escape ("\\");
                  when ASCII.BS => Escape ("\b");
                  when ASCII.FF => Escape ("\f");
                  when ASCII.LF => Escape ("\n");
                  when ASCII.CR => Escape ("\r");
                  when ASCII.HT => Escape ("\t");
                  when others =>
                     Escape ("\u00" & Hex (Character'Pos (C) / 16 + 1) &
                             Hex (Character'Pos (C) mod 16 + 1));
               end case;
               Run := I + 1;
            end if;
         end loop;
         if Run <= Length then
            Buffer (Pos .. Pos + Length - Run) := Text (Run .. Length);
            Pos := Pos + Length - Run + 1;
         end if;
      end if;
      Buffer (Pos) := '"';
      Pos := Pos + 1;
   end Put_Quoted;

   procedure Put_Quoted (Text : String) is
   begin
      if Text'Length = 0 then
         Put ("""""");
      else
         Put_Quoted (Text (Text'First)'Address, Text'Length);
      end if;
   end Put_Quoted;

   function Text return String is (Buffer (Body_Start .. Pos - 1));

   function Mark return Natural is (Pos);

   procedure Rewind (To : Natural) is
   begin
      Pos := To;
   end Rewind;

   function Reason (Status : Positive) return String is
     (case Status is
        when 200 => "OK", when 201 => "Created",
        when 400 => "Bad Request", when 401 => "Unauthorized",
        when 404 => "Not Found", when 413 => "Payload Too Large",
        when 503 => "Service Unavailable", when others => "Internal Server Error");

   function Copy_Out (First, Last : Natural) return System.Address is
      Block : constant System.Address := Pool.Take (Last - First + 1);
   begin
      Sys.Copy (Pool.Data (Block), Buffer (First)'Address,
                Interfaces.C.size_t (Last - First + 1));
      Pool.Set_Length (Block, Last - First + 1);
      return Block;
   end Copy_Out;

   function Finish (Status : Positive; Close : Boolean) return System.Address is
      Body_Length : constant Natural := Pos - Body_Start;
      Head : String (1 .. 160);
      H : Natural := 0;
      procedure Add (Text : String) is
      begin
         Head (H + 1 .. H + Text'Length) := Text;
         H := H + Text'Length;
      end Add;
      procedure Add_Number (Value : Natural) is
         Text : String (1 .. 10);
         N : Natural := Text'Last;
         V : Natural := Value;
      begin
         loop
            Text (N) := Character'Val (48 + V mod 10);
            N := N - 1;
            V := V / 10;
            exit when V = 0;
         end loop;
         Add (Text (N + 1 .. Text'Last));
      end Add_Number;
   begin
      Add ("HTTP/1.1 ");
      Add_Number (Status);
      Add (" ");
      Add (Reason (Status));
      Add (CRLF & "Content-Type: application/json" & CRLF & "Content-Length: ");
      Add_Number (Body_Length);
      if Close then
         Add (CRLF & "Connection: close" & CRLF & CRLF);
      else
         Add (CRLF & "Connection: keep-alive" & CRLF & "Keep-Alive: timeout=75" & CRLF & CRLF);
      end if;
      Buffer (Body_Start - H .. Body_Start - 1) := Head (1 .. H);
      return Copy_Out (Body_Start - H, Pos - 1);
   end Finish;

   function Raw (Text : String) return System.Address is
   begin
      Start;
      Put (Text);
      return Copy_Out (Body_Start, Pos - 1);
   end Raw;
end Output;
