with Ada.Strings.Unbounded;
with Application;

package HTTP is
   Max_Header : constant := 16_384;
   Max_Body : constant := 16_384;
   Max_Input : constant := 65_536;
   type Parse_Status is (Incomplete, Complete, Bad_Request, Too_Large);
   type Request is record
      Method, Path, Authorization, Payload :
        Ada.Strings.Unbounded.Unbounded_String;
      Close : Boolean := False;
      Expect_Continue : Boolean := False;
   end record;
   procedure Parse (Input : String; Item : out Request;
                    Status : out Parse_Status; Consumed : out Natural);
   function Serialize (Reply : Application.Response; Close : Boolean) return String;
end HTTP;
