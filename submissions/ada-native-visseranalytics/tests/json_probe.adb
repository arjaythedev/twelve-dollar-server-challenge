with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with JSON;

-- Line-oriented adapter for the independent Python JSON oracle.
procedure JSON_Probe is
   use type JSON.Value_Kind;
   Fields : JSON.Fields :=
     (1 => (To_Unbounded_String ("body"), JSON.Missing, Null_Unbounded_String));
   Root : JSON.Value_Kind;
begin
   while not Ada.Text_IO.End_Of_File loop
      declare
         -- Exercise non-one-based slices, as used by the request/auth parsers.
         Input : constant String := "prefix" & Ada.Text_IO.Get_Line;
      begin
         JSON.Parse (Input (7 .. Input'Last), Fields, Root);
         if Root /= JSON.Object_Value or else Fields (1).Kind /= JSON.String_Value then
            Ada.Text_IO.Put_Line ("INVALID");
         else
            declare
               Value : constant String := "prefix" & To_String (Fields (1).Text);
            begin
               Ada.Text_IO.Put_Line (JSON.Quote (Value (7 .. Value'Last)));
            end;
         end if;
      exception
         when JSON.Invalid_JSON => Ada.Text_IO.Put_Line ("INVALID");
      end;
   end loop;
end JSON_Probe;
