with Ada.Text_IO;
with JSON;
with Output;

-- Line-oriented adapter for the independent Python JSON oracle: parse the
-- document, unescape "body" and write it back with the response escaper.
procedure JSON_Probe is
   use type JSON.Value_Kind;
   Names : constant JSON.Name_Array := [1 => new String'("body")];
   Fields : JSON.Field_Array (1 .. 1);
   Root : JSON.Value_Kind;
begin
   while not Ada.Text_IO.End_Of_File loop
      declare
         -- Exercise non-one-based slices, as used by the request/auth parsers.
         Input : constant String := "prefix" & Ada.Text_IO.Get_Line;
      begin
         JSON.Parse (Input (7 .. Input'Last), Names, Fields, Root);
         if Root /= JSON.Object_Value or else Fields (1).Kind /= JSON.String_Value then
            Ada.Text_IO.Put_Line ("INVALID");
         else
            declare
               Value : String (101 .. 100 + Input'Length);
               Length : Natural;
            begin
               JSON.Decode (Input (7 .. Input'Last), Fields (1), Value, Length);
               Output.Start;
               Output.Put_Quoted (Value (101 .. 100 + Length));
               Ada.Text_IO.Put_Line (Output.Text);
            end;
         end if;
      exception
         when JSON.Invalid_JSON => Ada.Text_IO.Put_Line ("INVALID");
      end;
   end loop;
end JSON_Probe;
