with Ada.Directories;
with Interfaces.C; use Interfaces.C;
with Interfaces.C.Strings;
with System;
with Output;

package body Database is
   use type System.Address;
   use type Ada.Directories.File_Kind;
   package CS renames Interfaces.C.Strings;
   subtype Handle is System.Address;
   DB : Handle := System.Null_Address;
   type Query is (Health_Query, Feed_Query, Post_Query, Exists_Query,
                  Create_Query, Like_Query, Feed_IDs, Feed_Likes, Feed_Rows,
                  Begin_Query, Commit_Query, Rollback_Query);
   Statements : array (Query) of Handle := [others => System.Null_Address];
   Row : constant := 100;
   Done : constant := 101;

   -- Batch transaction state.
   Active : Boolean := False;   -- BEGIN executed, not yet committed
   Wrote : Boolean := False;    -- the active transaction holds writes
   Lost : Boolean := False;     -- writes of this batch were rolled back

   function SQLite_Open (Name : char_array; Result : access Handle;
                         Flags : int; VFS : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_open_v2";
   function SQLite_Close (Connection : Handle) return int
     with Import, Convention => C, External_Name => "sqlite3_close_v2";
   function Exec (Connection : Handle; SQL : char_array;
                  Callback, Arg, Error : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_exec";
   function Prepare (Connection : Handle; SQL : char_array; Bytes : int;
                     Flags : unsigned; Statement : access Handle;
                     Tail : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_prepare_v3";
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

   function Last_Error return String is
   begin
      if DB = System.Null_Address then return "database is not open"; end if;
      return CS.Value (Error_Message (DB));
   end Last_Error;

   procedure Require (RC : int; Expected : int := 0) is
   begin
      if RC /= Expected then
         raise Database_Error with Last_Error;
      end if;
   end Require;

   procedure Run (SQL : String) is
   begin
      Require (Exec (DB, To_C (SQL), System.Null_Address,
                     System.Null_Address, System.Null_Address));
   end Run;

   procedure Compile (Q : Query; SQL : String) is
      S : aliased Handle := System.Null_Address;
   begin
      -- SQLITE_PREPARE_PERSISTENT: these statements live as long as the server.
      Require (Prepare (DB, To_C (SQL), -1, 1, S'Access, System.Null_Address));
      Statements (Q) := S;
   end Compile;

   procedure Release (Q : Query) is
      Ignored : int;
   begin
      Ignored := Reset (Statements (Q));
   end Release;

   procedure Put_Text (S : Handle; Index : int) is
      Pointer : constant System.Address := Col_Text (S, Index);
      N : constant int := Col_Bytes (S, Index);
   begin
      if N = 0 or else Pointer = System.Null_Address then
         Output.Put ("""""");
      else
         Output.Put_Quoted (Pointer, Natural (N));
      end if;
   end Put_Text;

   function Column_Text (S : Handle; Index : int) return String is
      Pointer : constant System.Address := Col_Text (S, Index);
      N : constant int := Col_Bytes (S, Index);
   begin
      if N = 0 or else Pointer = System.Null_Address then return ""; end if;
      declare
         Value : String (1 .. Natural (N)) with Import, Address => Pointer;
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
      -- SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX: one thread uses it.
      Require (SQLite_Open (To_C (Path), Connection'Access, 2 + 32768,
                            System.Null_Address));
      DB := Connection;
      Require (Busy_Timeout (DB, 250));
      Require (Prepare (DB, To_C ("PRAGMA journal_mode=WAL"), -1, 0,
                        S'Access, System.Null_Address));
      Require (Step (S), Row);
      if Column_Text (S, 0) /= "wal" then
         Ignored := Finalize (S);
         raise Database_Error with "WAL mode required";
      end if;
      Require (Finalize (S));
      Run ("PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;" &
           "PRAGMA cache_size=-65536; PRAGMA mmap_size=0;" &
           "PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=4000;");
      Compile (Health_Query, "SELECT 1");
      Compile (Feed_Query, Select_Post &
               " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
      Compile (Feed_IDs, "SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20");
      Compile (Feed_Likes, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
      Compile (Feed_Rows, "SELECT p.id,p.body,p.created_at,u.username FROM posts p " &
               "JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2 " &
               "ORDER BY p.id DESC");
      Compile (Post_Query, Select_Post & " WHERE p.id=?1");
      Compile (Exists_Query, "SELECT 1 FROM posts WHERE id=?1");
      Compile (Create_Query, "INSERT INTO posts(user_id,body) VALUES(?1,?2)" &
               " RETURNING id,created_at");
      Compile (Like_Query, "INSERT INTO likes(user_id,post_id) VALUES(?1,?2)" &
               " ON CONFLICT(user_id,post_id) DO NOTHING");
      Compile (Begin_Query, "BEGIN");
      Compile (Commit_Query, "COMMIT");
      Compile (Rollback_Query, "ROLLBACK");
   end Open;

   procedure Close is
      Ignored : int;
   begin
      for S of Statements loop
         if S /= System.Null_Address then
            Ignored := Finalize (S);
            S := System.Null_Address;
         end if;
      end loop;
      if DB /= System.Null_Address then
         Ignored := SQLite_Close (DB);
         DB := System.Null_Address;
      end if;
   end Close;

   function In_Transaction return Boolean is (Active);

   procedure Ensure_Transaction is
   begin
      if not Active then
         Require (Step (Statements (Begin_Query)), Done);
         Release (Begin_Query);
         Active := True;
         Wrote := False;
      end if;
   end Ensure_Transaction;

   function Finish_Batch return Boolean is
      RC : int;
      OK : Boolean := not Lost;
   begin
      Lost := False;
      if not Active then
         return OK;
      end if;
      Active := False;
      RC := Step (Statements (Commit_Query));
      Release (Commit_Query);
      if RC /= Done then
         OK := OK and not Wrote;
         if Autocommit (DB) = 0 then
            RC := Step (Statements (Rollback_Query));
            Release (Rollback_Query);
         end if;
      end if;
      Wrote := False;
      return OK;
   end Finish_Batch;

   procedure Recover is
      Ignored : int;
   begin
      for S of Statements loop
         if S /= System.Null_Address then
            Ignored := Reset (S);
            Ignored := Clear (S);
         end if;
      end loop;
      -- Some errors make SQLite roll back the whole transaction.
      if Active and then Autocommit (DB) /= 0 then
         Active := False;
         Lost := Lost or Wrote;
         Wrote := False;
      end if;
   end Recover;

   procedure Check is
   begin
      Ensure_Transaction;
      Require (Step (Statements (Health_Query)), Row);
      Release (Health_Query);
   end Check;

   procedure Post_JSON (S : Handle; Like_Count : Long_Long_Integer) is
   begin
      Output.Put ("{""id"":");
      Output.Put_Int (Col_Int (S, 0));
      Output.Put (",""body"":");
      Put_Text (S, 1);
      Output.Put (",""created_at"":");
      Put_Text (S, 2);
      Output.Put (",""author"":");
      Put_Text (S, 3);
      Output.Put (",""like_count"":");
      Output.Put_Int (Like_Count);
      Output.Put ('}');
   end Post_JSON;

   procedure Original_Feed is
      S : constant Handle := Statements (Feed_Query);
      RC : int;
      First : Boolean := True;
   begin
      Output.Put ("{""posts"":[");
      loop
         RC := Step (S);
         exit when RC = Done;
         Require (RC, Row);
         if not First then Output.Put (','); end if;
         First := False;
         Post_JSON (S, Col_Int (S, 4));
      end loop;
      Release (Feed_Query);
      Output.Put ("]}");
   end Original_Feed;

   procedure Feed is
      Span : constant := 40;
      IDs : array (1 .. 20) of Long_Long_Integer := [others => 0];
      Likes : array (0 .. Span - 1) of Long_Long_Integer := [others => 0];
      Count, Next : Natural := 0;
      Newest, Oldest, ID : Long_Long_Integer;
      Ordered : Boolean := True;
      RC : int;
      S : Handle;
      Start : Natural;
   begin
      -- All scans share the batch's SQLite snapshot; nothing outlives it.
      Ensure_Transaction;
      S := Statements (Feed_IDs);
      loop
         RC := Step (S);
         exit when RC = Done;
         Require (RC, Row);
         Count := Count + 1;
         IDs (Count) := Col_Int (S, 0);
         if Count > 1 and then IDs (Count) >= IDs (Count - 1) then
            Ordered := False;
         end if;
      end loop;
      Release (Feed_IDs);
      if Count = 0 then
         Output.Put ("{""posts"":[]}");
         return;
      end if;
      Newest := IDs (1);
      Oldest := IDs (Count);
      -- Dense, newest-first IDs (the normal case) take three range scans.
      -- Anything else, such as backdated or externally edited rows, uses
      -- the reference query. The span test cannot overflow.
      if not Ordered or else Oldest > Long_Long_Integer'Last - Span or else
        Newest >= Oldest + Span
      then
         Original_Feed;
         return;
      end if;
      S := Statements (Feed_Likes);
      Require (Bind_Int (S, 1, Oldest));
      Require (Bind_Int (S, 2, Newest));
      loop
         RC := Step (S);
         exit when RC = Done;
         Require (RC, Row);
         ID := Col_Int (S, 0) - Oldest;
         Likes (Natural (ID)) := Likes (Natural (ID)) + 1;
      end loop;
      Release (Feed_Likes);
      Start := Output.Mark;
      Output.Put ("{""posts"":[");
      S := Statements (Feed_Rows);
      Require (Bind_Int (S, 1, Oldest));
      Require (Bind_Int (S, 2, Newest));
      Next := 1;
      loop
         RC := Step (S);
         exit when RC = Done;
         Require (RC, Row);
         ID := Col_Int (S, 0);
         if Next <= Count and then ID = IDs (Next) then
            if Next > 1 then Output.Put (','); end if;
            Post_JSON (S, Likes (Natural (ID - Oldest)));
            Next := Next + 1;
         elsif Next <= Count and then ID < IDs (Next) then
            -- A wanted post has no joined row (an orphan): start over.
            exit;
         end if;
         -- Otherwise the row is in range but not among the newest 20.
      end loop;
      Release (Feed_Rows);
      if Next <= Count then
         Output.Rewind (Start);
         Original_Feed;
         return;
      end if;
      Output.Put ("]}");
   end Feed;

   function Post (ID : Long_Long_Integer) return Boolean is
      S : constant Handle := Statements (Post_Query);
      RC : int;
   begin
      Ensure_Transaction;
      Require (Bind_Int (S, 1, ID));
      RC := Step (S);
      if RC = Row then
         Output.Put ("{""post"":");
         Post_JSON (S, Col_Int (S, 4));
         Output.Put ('}');
         Release (Post_Query);
         return True;
      end if;
      Require (RC, Done);
      Release (Post_Query);
      return False;
   end Post;

   procedure Create_Post (User_ID : Long_Long_Integer; Text, Username : String) is
      S : constant Handle := Statements (Create_Query);
      Static : constant System.Address := System.Null_Address;
      Empty : aliased constant String := " ";
      Start : constant Natural := Output.Mark;
   begin
      Ensure_Transaction;
      Require (Bind_Int (S, 1, User_ID));
      -- The text stays in place until the statement is reset below.
      Require (Bind_Text (S, 2, (if Text'Length = 0 then Empty'Address else Text'Address),
                          int (Text'Length), Static));
      Require (Step (S), Row);
      Output.Put ("{""post"":{""id"":");
      Output.Put_Int (Col_Int (S, 0));
      Output.Put (",""body"":");
      Output.Put_Quoted (Text);
      Output.Put (",""created_at"":");
      Put_Text (S, 1);
      Output.Put (",""author"":");
      Output.Put_Quoted (Username);
      Output.Put (",""like_count"":0}}");
      -- RETURNING's first row alone does not complete the statement.
      if Step (S) /= Done then
         Output.Rewind (Start);
         raise Database_Error with Last_Error;
      end if;
      Release (Create_Query);
      Wrote := True;
   end Create_Post;

   procedure Like (User_ID, Post_ID : Long_Long_Integer;
                   Found, Already : out Boolean) is
      S : Handle := Statements (Exists_Query);
      RC : int;
   begin
      Found := False;
      Already := False;
      Ensure_Transaction;
      Require (Bind_Int (S, 1, Post_ID));
      RC := Step (S);
      Release (Exists_Query);
      if RC = Done then
         return;
      end if;
      Require (RC, Row);
      Found := True;
      S := Statements (Like_Query);
      Require (Bind_Int (S, 1, User_ID));
      Require (Bind_Int (S, 2, Post_ID));
      Require (Step (S), Done);
      Already := Changes (DB) = 0;
      Release (Like_Query);
      if not Already then
         Wrote := True;
      end if;
   end Like;
end Database;
