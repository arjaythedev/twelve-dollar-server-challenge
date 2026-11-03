with HTTP;

package Application is
   procedure Initialize (Secret : String);
   -- Writes the body for one complete request with Output; returns the status.
   function Handle (Input : String; Item : HTTP.Request) return Positive;
   -- Writes {"error":Message} with Output.
   procedure Error_Body (Message : String);
end Application;
