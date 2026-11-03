with System;

-- Builds one response at a time in a reusable scratch buffer (the server is
-- single-threaded), then wraps it in HTTP framing inside a pool block.
package Output is
   procedure Start;
   procedure Put (Text : String) with Inline;
   procedure Put (Item : Character) with Inline;
   procedure Put_Int (Value : Long_Long_Integer);
   -- A JSON string literal, quotes included. Escapes '"', '\' and control
   -- characters; '/' and non-ASCII bytes are copied as-is.
   procedure Put_Quoted (Text : String);
   procedure Put_Quoted (Address : System.Address; Length : Natural);

   -- The body written since Start (for diagnostics and tests).
   function Text return String;
   function Mark return Natural with Inline;
   procedure Rewind (To : Natural) with Inline;

   -- Response block with status line, headers and the current body.
   function Finish (Status : Positive; Close : Boolean) return System.Address;
   -- A block holding Text verbatim (for example an interim 100 Continue).
   function Raw (Text : String) return System.Address;
end Output;
