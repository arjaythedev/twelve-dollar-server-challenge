with Ada.Strings.Unbounded;

package Application is
   type Response is record
      Status : Positive := 200;
      Content : Ada.Strings.Unbounded.Unbounded_String;
   end record;
   function Error (Status : Positive; Message : String) return Response;
   procedure Initialize (Secret : String);
   function Handle (Method, Path, Authorization, Payload : String) return Response;
end Application;
