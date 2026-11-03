with Ada.Command_Line;
with Ada.Environment_Variables;
with Ada.Numerics.Float_Random;
with Ada.Streams.Stream_IO;
with Ada.Text_IO;
with Interfaces.C;
with System;
with Application;
with Database;
with HTTP;
with Output;
with Pool;

-- CPU cost of the request path without the network: HTTP parsing, routing,
-- JWT verification, SQLite and response framing, in the load test's mix
-- (feed, post, 15% likes, 2% new posts) with a commit every Batch requests.
--   request_cost <iterations> <tokens.json> [batch]
-- SQLITE_PATH must name a private seed copy (it is written to).
procedure Request_Cost is
   use type HTTP.Parse_Status;
   use type Interfaces.C.int;
   package R renames Ada.Numerics.Float_Random;
   type Timespec is record
      Seconds, Nanoseconds : Interfaces.C.long;
   end record with Convention => C;
   function Get_Time (Clock : Interfaces.C.int; Stamp : access Timespec) return Interfaces.C.int
     with Import, Convention => C, External_Name => "clock_gettime";
   function CPU_Seconds return Long_Float is
      Stamp : aliased Timespec;
   begin
      if Get_Time (2, Stamp'Access) /= 0 then -- CLOCK_PROCESS_CPUTIME_ID
         raise Program_Error;
      end if;
      return Long_Float (Stamp.Seconds) + Long_Float (Stamp.Nanoseconds) / 1.0E9;
   end CPU_Seconds;

   Iterations : constant Positive := Positive'Value (Ada.Command_Line.Argument (1));
   Batch : constant Positive :=
     (if Ada.Command_Line.Argument_Count >= 3 then Positive'Value (Ada.Command_Line.Argument (3)) else 50);
   CRLF : constant String := ASCII.CR & ASCII.LF;
   Head : constant String := "Host: server" & CRLF & "User-Agent: Grafana k6/1.8.2" & CRLF;

   -- The first 200 tokens from seed/tokens.json.
   type Token_Text is record
      Text : String (1 .. 400);
      Length : Natural := 0;
   end record;
   Tokens : array (1 .. 200) of Token_Text;
   Token_Count : Natural := 0;

   procedure Load_Tokens (Path : String) is
      use Ada.Streams;
      File : Stream_IO.File_Type;
      Key : constant String := """token"":""";
      type Text_Access is access String;
      Data : constant Text_Access := new String (1 .. 262_144);
      Bytes : Stream_Element_Array (1 .. 262_144) with Import, Address => Data.all'Address;
      Last : Stream_Element_Offset;
      P : Natural := 1;
   begin
      Stream_IO.Open (File, Stream_IO.In_File, Path);
      Stream_IO.Read (File, Bytes, Last);
      Stream_IO.Close (File);
      while P + Key'Length <= Natural (Last) and then Token_Count < Tokens'Last loop
         if Data (P .. P + Key'Length - 1) = Key then
            P := P + Key'Length;
            declare
               E : Natural := P;
            begin
               while Data (E) /= '"' loop E := E + 1; end loop;
               Token_Count := Token_Count + 1;
               Tokens (Token_Count).Length := E - P;
               Tokens (Token_Count).Text (1 .. E - P) := Data (P .. E - 1);
               P := E;
            end;
         else
            P := P + 1;
         end if;
      end loop;
   end Load_Tokens;

   Gen : R.Generator;
   Requests, Handled : Natural := 0;
   Newest : Natural := 500_000;
   Started, Finished : Long_Float;

   procedure Serve (Wire : String) is
      Item : HTTP.Request;
      Status : HTTP.Parse_Status;
      Code : Positive;
      Block : System.Address;
   begin
      HTTP.Parse (Wire, Item, Status);
      if Status /= HTTP.Complete then
         raise Program_Error with "incomplete request";
      end if;
      Code := Application.Handle (Wire, Item);
      if Code >= 400 then
         raise Program_Error with "status" & Code'Image & ": " & Output.Text;
      end if;
      Block := Output.Finish (Code, False);
      Pool.Give (Block);
      Requests := Requests + 1;
      Handled := Handled + 1;
      if Handled = Batch then
         if not Database.Finish_Batch then
            raise Program_Error with "commit failed";
         end if;
         Handled := 0;
      end if;
   end Serve;

   function Image (N : Natural) return String is
      S : constant String := N'Image;
   begin
      return S (S'First + 1 .. S'Last);
   end Image;

   procedure One_User is
      ID : constant Natural := Newest - Natural (R.Random (Gen) * 19.0);
      T : constant Positive := 1 + Natural (R.Random (Gen) * Float (Token_Count - 1));
      Bearer : constant String := "Authorization: Bearer " &
        Tokens (T).Text (1 .. Tokens (T).Length) & CRLF;
   begin
      Serve ("GET /feed HTTP/1.1" & CRLF & Head & CRLF);
      Serve ("GET /posts/" & Image (ID) & " HTTP/1.1" & CRLF & Head & CRLF);
      if R.Random (Gen) < 0.15 then
         Serve ("POST /posts/" & Image (ID) & "/like HTTP/1.1" & CRLF & Head & Bearer &
                "Content-Type: application/json" & CRLF & CRLF);
      end if;
      if R.Random (Gen) < 0.02 then
         declare
            Payload : constant String := "{""body"":""bench user says hi at 2026-10-28T12:00:00.000Z (VU 1, iter " &
              Image (Requests) & ")""}";
         begin
            Serve ("POST /posts HTTP/1.1" & CRLF & Head & Bearer & "Content-Type: application/json" & CRLF &
                   "Content-Length: " & Image (Payload'Length) & CRLF & CRLF & Payload);
            Newest := Newest + 1;
         end;
      end if;
   end One_User;
begin
   R.Reset (Gen, 20261028);
   Load_Tokens (Ada.Command_Line.Argument (2));
   Database.Open (Ada.Environment_Variables.Value ("SQLITE_PATH"));
   Application.Initialize (Ada.Environment_Variables.Value ("JWT_SECRET", "twelve-dollar-challenge"));
   for I in 1 .. 2_000 loop
      One_User;
   end loop;
   Requests := 0;
   Started := CPU_Seconds;
   for I in 1 .. Iterations loop
      One_User;
   end loop;
   Finished := CPU_Seconds;
   if not Database.Finish_Batch then
      raise Program_Error with "commit failed";
   end if;
   Ada.Text_IO.Put_Line ("requests" & Requests'Image & " cpu_us_per_request" &
     Long_Float'Image ((Finished - Started) * 1.0E6 / Long_Float (Requests)));
   Database.Close;
end Request_Cost;
