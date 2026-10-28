with Ada.Directories;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces.C; use Interfaces.C;
with Interfaces.C.Strings;
with System;
with System.Storage_Elements;
with JSON;

package body Database is
   use type System.Address;
   use type Ada.Directories.File_Kind;
   package CS renames Interfaces.C.Strings;
   subtype Handle is System.Address;
   DB : Handle := System.Null_Address;
   type Query is (Health_Query, Feed_Query, Post_Query, Exists_Query,
                  Create_Query, Like_Query);
   Statements : array (Query) of Handle := (others => System.Null_Address);

   function SQLite_Open (Name : char_array; Result : access Handle;
                         Flags : int; VFS : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_open_v2";
   function SQLite_Close (Connection : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_close_v2";
   function Exec (Connection : Handle; SQL : char_array;
                  Callback, Arg, Error : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_exec";
   function Prepare (Connection : Handle; SQL : char_array; Bytes : int;
                     Statement : access Handle; Tail : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_prepare_v2";
   function Step (Statement : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_step";
   function Reset (Statement : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_reset";
   function Clear (Statement : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_clear_bindings";
   function Finalize (Statement : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_finalize";
   function Bind_Int (Statement : Handle; Index : int;
                      Value : Long_Long_Integer) return int
     with Import, Convention => C, External_Name => "sqlite3_bind_int64";
   function Bind_Text (Statement : Handle; Index : int; Text : System.Address;
                       Bytes : int; Destructor : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_bind_text";
   function Col_Int (Statement : Handle; Index : int) return Long_Long_Integer
     with Import, Convention => C, External_Name => "sqlite3_column_int64";
   function Col_Text (Statement : Handle; Index : int) return System.Address
     with Import, Convention => C, External_Name => "sqlite3_column_text";
   function Col_Bytes (Statement : Handle; Index : int) return int
     with Import, Convention => C, External_Name => "sqlite3_column_bytes";
   function Changes (Connection : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_changes";
   function Autocommit (Connection : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_get_autocommit";
   function Error_Message (Connection : Handle) return CS.chars_ptr
     with Import, Convention => C, External_Name => "sqlite3_errmsg";
   function Busy_Timeout (Connection : Handle; Milliseconds : int) return int
     with Import, Convention => C, External_Name => "sqlite3_busy_timeout";

   procedure Require (RC : int; Expected : int := 0) is
   begin
      if RC /= Expected then raise Database_Error with Last_Error; end if;
   end Require;

   function Last_Error return String is
   begin
      if DB = System.Null_Address then return "database is not open"; end if;
      return CS.Value (Error_Message (DB));
   end Last_Error;

   procedure Run (SQL : String) is
   begin
      Require (Exec (DB, To_C (SQL), System.Null_Address,
                     System.Null_Address, System.Null_Address));
   end Run;

   procedure Compile (Q : Query; SQL : String) is
      S : aliased Handle := System.Null_Address;
   begin
      Require (Prepare (DB, To_C (SQL), -1, S'Access, System.Null_Address));
      Statements (Q) := S;
   end Compile;

   procedure Release (Q : Query) is
      Ignored : int;
   begin
      Ignored := Reset (Statements (Q));
      Require (Clear (Statements (Q)));
   end Release;

   procedure Recover is
      Ignored : int;
   begin
      for Q in Query loop
         if Statements (Q) /= System.Null_Address then
            Ignored := Reset (Statements (Q));
            Ignored := Clear (Statements (Q));
         end if;
      end loop;
      if DB /= System.Null_Address and then Autocommit (DB) = 0 then
         Run ("ROLLBACK");
      end if;
   end Recover;

   procedure Close is
      Ignored : int;
   begin
      for S of Statements loop
         if S /= System.Null_Address then
            Ignored := Finalize (S); S := System.Null_Address;
         end if;
      end loop;
      if DB /= System.Null_Address then
         Ignored := SQLite_Close (DB); DB := System.Null_Address;
      end if;
   end Close;

   function Column_Text (S : Handle; Index : int) return String is
      Pointer : constant System.Address := Col_Text (S, Index);
      N : constant int := Col_Bytes (S, Index);
   begin
      if N = 0 then return ""; end if;
      declare
         Value : String (1 .. Natural (N))
           with Import, Address => Pointer;
      begin
         return Value;
      end;
   end Column_Text;

   procedure Open (Path : String) is
      Connection : aliased Handle := System.Null_Address;
      S : aliased Handle := System.Null_Address;
      Ignored : int;
      Select_Post : constant String :=
        "SELECT p.id,p.body,p.created_at,u.username," &
        "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) " &
        "FROM posts p JOIN users u ON u.id=p.user_id";
   begin
      -- No CREATE or URI flag: never accidentally open a temporary/memory DB.
      if Path = "" or else Path = ":memory:" or else
        not Ada.Directories.Exists (Path) or else
        Ada.Directories.Kind (Path) /= Ada.Directories.Ordinary_File
      then
         raise Database_Error with "SQLITE_PATH must name an existing file";
      end if;
      Require (SQLite_Open (To_C (Path), Connection'Access, 2 + 32768,
                            System.Null_Address));
      DB := Connection;
      Require (Busy_Timeout (DB, 250));
      Require (Prepare (DB, To_C ("PRAGMA journal_mode=WAL"), -1,
                        S'Access, System.Null_Address));
      Require (Step (S), 100);
      if Column_Text (S, 0) /= "wal" then
         Ignored := Finalize (S);
         raise Database_Error with "WAL mode required";
      end if;
      Require (Finalize (S));
      Run ("PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;" &
           "PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;" &
           "PRAGMA wal_autocheckpoint=1000;");
      Compile (Health_Query, "SELECT 1");
      Compile (Feed_Query, Select_Post &
               " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
      Compile (Post_Query, Select_Post & " WHERE p.id=?1");
      Compile (Exists_Query, "SELECT 1 FROM posts WHERE id=?1");
      Compile (Create_Query, "INSERT INTO posts(user_id,body) VALUES(?1,?2)" &
               " RETURNING id,created_at");
      Compile (Like_Query, "INSERT INTO likes(user_id,post_id) VALUES(?1,?2)" &
               " ON CONFLICT(user_id,post_id) DO NOTHING");
   end Open;

   procedure Check is
   begin
      Require (Step (Statements (Health_Query)), 100);
      Release (Health_Query);
   end Check;

   function Post_JSON (S : Handle) return String is
     ("{""id"":" & JSON.Number (Col_Int (S, 0)) &
      ",""body"":" & JSON.Quote (Column_Text (S, 1)) &
      ",""created_at"":" & JSON.Quote (Column_Text (S, 2)) &
      ",""author"":" & JSON.Quote (Column_Text (S, 3)) &
      ",""like_count"":" & JSON.Number (Col_Int (S, 4)) & "}");

   function Feed return String is
      Result : Unbounded_String := To_Unbounded_String ("{""posts"":[");
      S : constant Handle := Statements (Feed_Query);
      RC : int;
      First : Boolean := True;
   begin
      loop
         RC := Step (S);
         exit when RC = 101;
         Require (RC, 100);
         if not First then Append (Result, ","); end if;
         First := False;
         Append (Result, Post_JSON (S));
      end loop;
      Release (Feed_Query);
      Append (Result, "]}");
      return To_String (Result);
   end Feed;

   function Post (ID : Long_Long_Integer) return String is
      S : constant Handle := Statements (Post_Query);
      RC : int;
      Result : Unbounded_String;
   begin
      Require (Bind_Int (S, 1, ID));
      RC := Step (S);
      if RC = 100 then
         Result := To_Unbounded_String ("{""post"":" & Post_JSON (S) & "}");
      else Require (RC, 101);
      end if;
      Release (Post_Query);
      return To_String (Result);
   end Post;

   function Create_Post (User_ID : Long_Long_Integer; Text, Username : String)
                         return String is
      S : constant Handle := Statements (Create_Query);
      Result : Unbounded_String;
      Transient : constant System.Address :=
        System.Storage_Elements.To_Address
          (System.Storage_Elements.Integer_Address'Last);
   begin
      Run ("BEGIN IMMEDIATE");
      Require (Bind_Int (S, 1, User_ID));
      Require (Bind_Text (S, 2, Text'Address, int (Text'Length), Transient));
      Require (Step (S), 100);
      Result := To_Unbounded_String
        ("{""post"":{""id"":" & JSON.Number (Col_Int (S, 0)) &
         ",""body"":" & JSON.Quote (Text) &
         ",""created_at"":" & JSON.Quote (Column_Text (S, 1)) &
         ",""author"":" & JSON.Quote (Username) & ",""like_count"":0}}");
      -- RETURNING's first row alone does not complete/commit the statement.
      Require (Step (S), 101);
      Release (Create_Query);
      Run ("COMMIT");
      return To_String (Result);
   end Create_Post;

   procedure Like (User_ID, Post_ID : Long_Long_Integer;
                   Found, Already : out Boolean) is
      S : Handle := Statements (Exists_Query);
      RC : int;
   begin
      Found := False; Already := False;
      Run ("BEGIN IMMEDIATE");
      Require (Bind_Int (S, 1, Post_ID));
      RC := Step (S);
      if RC = 101 then
         Release (Exists_Query); Run ("ROLLBACK"); return;
      end if;
      Require (RC, 100);
      Release (Exists_Query);
      Found := True;
      S := Statements (Like_Query);
      Require (Bind_Int (S, 1, User_ID));
      Require (Bind_Int (S, 2, Post_ID));
      Require (Step (S), 101);
      Already := Changes (DB) = 0;
      Release (Like_Query);
      Run ("COMMIT");
   end Like;
end Database;
