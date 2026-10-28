-- HTTP/1.1 request framing, parsed in place. Fields are index ranges into
-- the caller's input; only chunked bodies are copied (into Chunk_Data).
package HTTP is
   Max_Header : constant := 16_384;
   Max_Body : constant := 16_384;
   Max_Input : constant := 65_536;
   type Parse_Status is (Incomplete, Complete, Bad_Request, Too_Large);
   type Method_Kind is (Get, Post, Other);
   type Request is record
      Method : Method_Kind := Other;
      Path_First : Positive := 1;
      Path_Last : Natural := 0;
      Has_Auth : Boolean := False;
      Auth_First : Positive := 1;
      Auth_Last : Natural := 0;
      -- Payload: Input (Body_First .. Body_Last), or Chunk_Data when Chunked.
      Chunked : Boolean := False;
      Body_First : Positive := 1;
      Body_Last : Natural := 0;
      Close : Boolean := False;
      Expect_Continue : Boolean := False;
      -- Bytes of Input used by a Complete request.
      Consumed : Natural := 0;
   end record;

   Chunk_Data : String (1 .. Max_Body);
   Chunk_Length : Natural := 0;

   procedure Parse (Input : String; Item : out Request; Status : out Parse_Status);
end HTTP;
