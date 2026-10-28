with Ada.Command_Line;
with Ada.Environment_Variables;
with Ada.Execution_Time;
with Ada.Real_Time;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Application;
with Database;
with JSON;
with HTTP;

-- Diagnostic CPU cost only: no HTTP transport, concurrency or capacity claim.
-- SQLITE_PATH must point to a private seed copy, just like the server.
procedure Request_Cost is
   use type Ada.Execution_Time.CPU_Time;
   Mode : constant String := Ada.Command_Line.Argument (1);
   Iterations : constant Positive := Positive'Value (Ada.Command_Line.Argument (2));
   Text : constant String (1 .. 200) := (others => 'a');
   Document : constant String := "{""body"":""" & Text & """}";
   Fields : JSON.Fields :=
     (1 => (To_Unbounded_String ("body"), JSON.Missing, Null_Unbounded_String));
   Root : JSON.Value_Kind;
   Bytes : Long_Long_Integer := 0;
   Started : Ada.Execution_Time.CPU_Time;
   CRLF : constant String := ASCII.CR & ASCII.LF;
   Wire : constant String := "GET /feed HTTP/1.1" & CRLF &
     "Host: server" & CRLF & "User-Agent: k6/1.8.2" & CRLF &
     "Accept-Encoding: gzip" & CRLF & CRLF;

   procedure Work is
   begin
      if Mode = "quote" then
         declare
            Result : constant String := JSON.Quote (Text);
         begin
            Bytes := Bytes + Result'Length;
         end;
      elsif Mode = "parse" then
         JSON.Parse (Document, Fields, Root);
         Bytes := Bytes + Long_Long_Integer (Length (Fields (1).Text));
      elsif Mode in "headers" | "wire" then
         declare
            Item : HTTP.Request;
            Status : HTTP.Parse_Status;
            Consumed : Natural;
            use type HTTP.Parse_Status;
         begin
            HTTP.Parse (Wire, Item, Status, Consumed);
            if Status /= HTTP.Complete or else Consumed /= Wire'Length then
               raise Program_Error;
            end if;
            if Mode = "wire" then
               declare
                  Serialized : constant String := HTTP.Serialize
                    (Application.Handle (To_String (Item.Method), To_String (Item.Path),
                     To_String (Item.Authorization), To_String (Item.Payload)), Item.Close);
               begin
                  Bytes := Bytes + Serialized'Length;
               end;
            else
               Bytes := Bytes + Long_Long_Integer (Consumed);
            end if;
         end;
      else
         declare
            Result : constant Application.Response := Application.Handle
              ("GET", (if Mode = "feed" then "/feed" else "/posts/1"), "", "");
         begin
            if Result.Status /= 200 then raise Program_Error; end if;
            Bytes := Bytes + Long_Long_Integer (Length (Result.Content));
         end;
      end if;
   end Work;
begin
   if Mode not in "quote" | "parse" | "feed" | "post" | "headers" | "wire" then
      raise Constraint_Error with "expected quote, parse, feed, post, headers or wire";
   end if;
   Database.Open (Ada.Environment_Variables.Value ("SQLITE_PATH"));
   Application.Initialize ("request-cost-diagnostic");
   for I in 1 .. 1_000 loop Work; end loop;
   Bytes := 0;
   Started := Ada.Execution_Time.Clock;
   for I in 1 .. Iterations loop Work; end loop;
   declare
      Elapsed : constant Duration := Ada.Real_Time.To_Duration
        (Ada.Execution_Time.Clock - Started);
   begin
      Ada.Text_IO.Put_Line
        ("{""mode"":" & JSON.Quote (Mode) & ",""iterations"":" &
         JSON.Number (Long_Long_Integer (Iterations)) & ",""cpu_seconds"":" &
         Duration'Image (Elapsed) & ",""bytes"":" & JSON.Number (Bytes) & "}");
   end;
   Database.Close;
end Request_Cost;
