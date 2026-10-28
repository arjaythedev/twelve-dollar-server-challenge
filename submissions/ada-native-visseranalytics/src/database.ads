with Ada.Strings.Unbounded;

package Database is
   Database_Error : exception;
   procedure Open (Path : String);
   procedure Close;
   procedure Check;
   procedure Recover;
   function Feed return String;
   function Post (ID : Long_Long_Integer) return String;
   function Create_Post (User_ID : Long_Long_Integer; Text, Username : String)
                         return String;
   procedure Like (User_ID, Post_ID : Long_Long_Integer;
                   Found, Already : out Boolean);
   function Last_Error return String;
end Database;
