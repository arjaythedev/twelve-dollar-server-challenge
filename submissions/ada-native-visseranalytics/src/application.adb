with Ada.Exceptions;
with Ada.Real_Time;
with Ada.Strings.Fixed;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Auth;
with Database;
with JSON;

package body Application is
   use type Ada.Real_Time.Time;
   use type JSON.Value_Kind;
   use type Auth.Status;
   Secret_Key : Unbounded_String;
   Started : Ada.Real_Time.Time;

   procedure Initialize (Secret : String) is
   begin
      Secret_Key := To_Unbounded_String (Secret);
      Started := Ada.Real_Time.Clock;
   end Initialize;

   function Error (Status : Positive; Message : String) return Response is
     ((Status, To_Unbounded_String ("{""error"":" & JSON.Quote (Message) & "}")));

   function Handle (Method, Path, Authorization, Payload : String) return Response is
      Is_Create : constant Boolean := Method = "POST" and then Path = "/posts";
      Is_Like : constant Boolean := Method = "POST" and then Path'Length >= 12
        and then Path (Path'First .. Path'First + 6) = "/posts/"
        and then Path (Path'Last - 4 .. Path'Last) = "/like"
        and then Ada.Strings.Fixed.Index
          (Path (Path'First + 7 .. Path'Last - 5), "/") = 0;
      Is_Post : constant Boolean := Method = "GET" and then Path'Length >= 7
        and then Path (Path'First .. Path'First + 6) = "/posts/"
        and then Ada.Strings.Fixed.Index
          (Path (Path'First + 7 .. Path'Last), "/") = 0;
      ID, User_ID : Long_Long_Integer := 0;
      Username : Unbounded_String;
      Auth_Result : Auth.Status;
      Found, Already : Boolean;
   begin
      if Method = "GET" and then Path = "/health" then
         begin
            Database.Check;
            return (200, To_Unbounded_String
              ("{""status"":""ok"",""db"":""ok"",""uptime_s"":" &
               JSON.Number (Long_Long_Integer
                 (Long_Long_Float'Floor (Long_Long_Float (Ada.Real_Time.To_Duration
                   (Ada.Real_Time.Clock - Started))))) & "}"));
         exception
            when Database.Database_Error =>
               declare
                  Message : constant String := Database.Last_Error;
               begin
                  Database.Recover;
                  return (503, To_Unbounded_String
                    ("{""status"":""degraded"",""db"":""unreachable"",""error"":" &
                     JSON.Quote (Message) & "}"));
               end;
         end;
      elsif Method = "GET" and then Path = "/feed" then
         return (200, To_Unbounded_String (Database.Feed));
      elsif not (Is_Create or Is_Like or Is_Post) then
         return Error (404, "not found");
      end if;

      if Is_Create or Is_Like then
         Auth.Verify (Authorization, To_String (Secret_Key), Auth_Result,
                      User_ID, Username);
         case Auth_Result is
            when Auth.Missing_Bearer => return Error (401, "missing bearer token");
            when Auth.Invalid_Token => return Error (401, "invalid or expired token");
            when Auth.Invalid_Payload => return Error (401, "invalid token payload");
            when Auth.Valid => null;
         end case;
      end if;
      if Is_Post or Is_Like then
         ID := JSON.Positive_ID (Path (Path'First + 7 ..
           Path'Last - (if Is_Like then 5 else 0)));
         if ID = 0 then return Error (400, "invalid post id"); end if;
      end if;
      if Is_Post then
         declare
            Result : constant String := Database.Post (ID);
         begin
            if Result = "" then return Error (404, "post not found"); end if;
            return (200, To_Unbounded_String (Result));
         end;
      elsif Is_Like then
         Database.Like (User_ID, ID, Found, Already);
         if not Found then return Error (404, "post not found"); end if;
         return ((if Already then 200 else 201), To_Unbounded_String
           ("{""liked"":true,""already_liked"":" &
            (if Already then "true" else "false") &
            ",""post_id"":" & JSON.Number (ID) & "}"));
      else
         declare
            Fields : JSON.Fields :=
              (1 => (To_Unbounded_String ("body"), JSON.Missing, Null_Unbounded_String));
            Root : JSON.Value_Kind;
         begin
            begin
               JSON.Parse (Payload, Fields, Root);
            exception
               when JSON.Invalid_JSON => return Error (400, "malformed JSON body");
            end;
            if Root /= JSON.Object_Value or else Fields (1).Kind /= JSON.String_Value then
               return Error (400, "body is required");
            end if;
            declare
               Text : constant String := JSON.Trim (To_String (Fields (1).Text));
            begin
               if Text'Length = 0 then return Error (400, "body is required"); end if;
               if JSON.Character_Count (Text) > 500 then
                  return Error (400, "body must be at most 500 characters");
               end if;
               return (201, To_Unbounded_String
                 (Database.Create_Post (User_ID, Text, To_String (Username))));
            end;
         end;
      end if;
   exception
      when E : others =>
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error,
           "request failed: " & Ada.Exceptions.Exception_Information (E));
         Database.Recover;
         return Error (500, "internal server error");
   end Handle;
end Application;
