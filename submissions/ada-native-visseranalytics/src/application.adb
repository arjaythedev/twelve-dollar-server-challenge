with Ada.Exceptions;
with Ada.Text_IO;
with Interfaces;
with Auth;
with Database;
with JSON;
with Output;
with Sys;

package body Application is
   use type JSON.Value_Kind;
   use type Auth.Status;
   use type HTTP.Method_Kind;
   use type Interfaces.Unsigned_32;

   Started : Interfaces.Unsigned_32 := 0;
   Body_Name : aliased constant String := "body";
   Body_Names : constant JSON.Name_Array := [1 => Body_Name'Access];
   Body_Text : String (1 .. HTTP.Max_Body);

   procedure Initialize (Secret : String) is
   begin
      Auth.Initialize (Secret);
      Started := Sys.Monotonic_Seconds;
   end Initialize;

   procedure Error_Body (Message : String) is
   begin
      Output.Put ("{""error"":");
      Output.Put_Quoted (Message);
      Output.Put ('}');
   end Error_Body;

   function Error (Status : Positive; Message : String) return Positive is
   begin
      Output.Start;
      Error_Body (Message);
      return Status;
   end Error;

   function Health return Positive is
   begin
      Database.Check;
      Output.Put ("{""status"":""ok"",""db"":""ok"",""uptime_s"":");
      Output.Put_Int (Long_Long_Integer (Sys.Monotonic_Seconds - Started));
      Output.Put ('}');
      return 200;
   exception
      when Database.Database_Error =>
         declare
            Message : constant String := Database.Last_Error;
         begin
            Database.Recover;
            Output.Start;
            Output.Put ("{""status"":""degraded"",""db"":""unreachable"",""error"":");
            Output.Put_Quoted (Message);
            Output.Put ('}');
            return 503;
         end;
   end Health;

   function Create (Payload : String; User_ID : Long_Long_Integer) return Positive is
      Fields : JSON.Field_Array (1 .. 1);
      Root : JSON.Value_Kind;
      Length : Natural;
      First : Positive;
      Last : Natural;
   begin
      begin
         JSON.Parse (Payload, Body_Names, Fields, Root);
      exception
         when JSON.Invalid_JSON => return Error (400, "malformed JSON body");
      end;
      if Root /= JSON.Object_Value or else Fields (1).Kind /= JSON.String_Value then
         return Error (400, "body is required");
      end if;
      JSON.Decode (Payload, Fields (1), Body_Text, Length);
      JSON.Trim (Body_Text (1 .. Length), First, Last);
      if Last < First then
         return Error (400, "body is required");
      end if;
      if JSON.Character_Count (Body_Text (First .. Last)) > 500 then
         return Error (400, "body must be at most 500 characters");
      end if;
      Database.Create_Post (User_ID, Body_Text (First .. Last),
                            Auth.Username (1 .. Auth.Username_Length));
      return 201;
   end Create;

   function Handle (Input : String; Item : HTTP.Request) return Positive is
      Path : String renames Input (Item.Path_First .. Item.Path_Last);
      Is_Create : constant Boolean := Item.Method = HTTP.Post and then Path = "/posts";
      Is_Like : constant Boolean := Item.Method = HTTP.Post and then Path'Length >= 12
        and then Path (Path'First .. Path'First + 6) = "/posts/"
        and then Path (Path'Last - 4 .. Path'Last) = "/like"
        and then (for all C of Path (Path'First + 7 .. Path'Last - 5) => C /= '/');
      Is_Post : constant Boolean := Item.Method = HTTP.Get and then Path'Length >= 7
        and then Path (Path'First .. Path'First + 6) = "/posts/"
        and then (for all C of Path (Path'First + 7 .. Path'Last) => C /= '/');
      ID, User_ID : Long_Long_Integer := 0;
      Verdict : Auth.Status;
      Found, Already : Boolean;
   begin
      Output.Start;
      if Item.Method = HTTP.Get and then Path = "/health" then
         return Health;
      elsif Item.Method = HTTP.Get and then Path = "/feed" then
         Database.Feed;
         return 200;
      elsif not (Is_Create or Is_Like or Is_Post) then
         return Error (404, "not found");
      end if;

      if Is_Create or Is_Like then
         Auth.Verify (Item.Has_Auth, Input (Item.Auth_First .. Item.Auth_Last),
                      Verdict, User_ID);
         case Verdict is
            when Auth.Missing_Bearer => return Error (401, "missing bearer token");
            when Auth.Invalid_Token => return Error (401, "invalid or expired token");
            when Auth.Invalid_Payload => return Error (401, "invalid token payload");
            when Auth.Valid => null;
         end case;
      end if;
      if Is_Post or Is_Like then
         ID := JSON.Positive_ID (Path (Path'First + 7 .. Path'Last - (if Is_Like then 5 else 0)));
         if ID = 0 then return Error (400, "invalid post id"); end if;
      end if;

      if Is_Post then
         if not Database.Post (ID) then
            return Error (404, "post not found");
         end if;
         return 200;
      elsif Is_Like then
         Database.Like (User_ID, ID, Found, Already);
         if not Found then
            return Error (404, "post not found");
         end if;
         Output.Put ("{""liked"":true,""already_liked"":");
         Output.Put (if Already then "true" else "false");
         Output.Put (",""post_id"":");
         Output.Put_Int (ID);
         Output.Put ('}');
         return (if Already then 200 else 201);
      elsif Item.Chunked then
         return Create (HTTP.Chunk_Data (1 .. HTTP.Chunk_Length), User_ID);
      else
         return Create (Input (Item.Body_First .. Item.Body_Last), User_ID);
      end if;
   exception
      when E : others =>
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error,
           "request failed: " & Ada.Exceptions.Exception_Information (E));
         Database.Recover;
         return Error (500, "internal server error");
   end Handle;
end Application;
