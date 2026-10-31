with Ada.Directories;
with Ada.Unchecked_Conversion;
with Interfaces; use Interfaces;
with Interfaces.C; use Interfaces.C;
with Interfaces.C.Strings;
with System;
with System.Storage_Elements;
with Checkpointer;
with Output;
with Sys;

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

   -- WAL restarts. The checkpoint helper copies WAL frames into the
   -- database file, but only a writer can restart the WAL, and only when
   -- every frame has been copied as its read transaction begins. Under a
   -- steady stream of writes that moment never comes, so past Gate_Frames
   -- the server holds write requests (reads go on), asks the helper for a
   -- pass, and releases them once the helper has caught up: the first of
   -- them then restarts the WAL. Without a responsive helper, or past
   -- Inline_Frames, the server checkpoints inline as SQLite would.
   Gate_Frames : constant := 4_000;
   Inline_Frames : constant := 16_000;
   Gate_Timeout : constant := 1_000;  -- ms
   Holding : Boolean := False;
   Ticket : Unsigned_32 := 0;
   Gate_Start : Unsigned_64 := 0;
   Gates, Gate_Timeouts, Inline_Checkpoints : Natural := 0;
   Worst_Gate : Unsigned_64 := 0;  -- ms writes were held, longest gate

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
   function Set_WAL_Hook (Connection : Handle; Callback, Arg : System.Address) return System.Address
     with Import, Convention => C, External_Name => "sqlite3_wal_hook";
   function Checkpoint (Connection : Handle; Name : System.Address; Mode : int;
                        Log, Done : access int) return int
     with Import, Convention => C, External_Name => "sqlite3_wal_checkpoint_v2";

   -- Called by SQLite after each commit that wrote to the WAL.
   function On_Commit (Arg : System.Address; Connection : Handle;
                       Name : System.Address; Frames : int) return int
     with Convention => C;

   function On_Commit (Arg : System.Address; Connection : Handle;
                       Name : System.Address; Frames : int) return int
   is
      pragma Unreferenced (Arg, Name);
      Log, Done : aliased int := 0;
      Ignored : int;
   begin
      if Frames >= Inline_Frames
        or else (Frames >= Gate_Frames and then not Checkpointer.Available)
      then
         -- PASSIVE, as SQLite's own automatic checkpoint.
         Inline_Checkpoints := Inline_Checkpoints + 1;
         Ignored := Checkpoint (Connection, System.Null_Address, 0, Log'Access, Done'Access);
      elsif Frames >= Gate_Frames and then not Holding then
         Holding := True;
         Ticket := Checkpointer.Request_Pass;
         Gate_Start := Sys.Monotonic_Milliseconds;
      end if;
      return 0;
   end On_Commit;

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

   ---------------------------------------------------------------------------
   -- Direct B-tree reads
   --
   -- GET /feed and GET /posts/:id, nearly all of the load, read their rows
   -- through SQLite's own B-tree cursors (src/sqlite_btree.c) instead of
   -- compiled statements: the same pages, page cache and WAL snapshot, inside
   -- the same batch transaction, without the bytecode interpreter. The
   -- records are decoded below. Anything unexpected (a missing row, a value
   -- of another type, a record not stored whole on its page, any error)
   -- sends the request through the SQL statements instead, which stay the
   -- reference. Open enables this only for the schema it was written for.
   ---------------------------------------------------------------------------

   function BT_Setup (Connection : Handle; Cursor : int; Root : unsigned;
                      Fields, Descending : int) return int
     with Import, Convention => C, External_Name => "ada_bt_setup";
   function BT_Begin return int
     with Import, Convention => C, External_Name => "ada_bt_begin";
   procedure BT_End
     with Import, Convention => C, External_Name => "ada_bt_end";
   function BT_Seek_Row (Cursor : int; Row_ID : Long_Long_Integer) return int
     with Import, Convention => C, External_Name => "ada_bt_seek_row";
   function BT_Seek_Int (Cursor : int; Key : Long_Long_Integer) return int
     with Import, Convention => C, External_Name => "ada_bt_seek_int";
   function BT_First (Cursor : int) return int
     with Import, Convention => C, External_Name => "ada_bt_first";
   function BT_Step (Cursor : int; Backward : int) return int
     with Import, Convention => C, External_Name => "ada_bt_step";
   function BT_Row_ID (Cursor : int) return Long_Long_Integer
     with Import, Convention => C, External_Name => "ada_bt_rowid";
   function BT_Record (Cursor : int; Length : access unsigned) return System.Address
     with Import, Convention => C, External_Name => "ada_bt_record";
   procedure BT_Close
     with Import, Convention => C, External_Name => "ada_bt_close";

   Posts_Table : constant int := 0;   -- (id, user_id, body, created_at)
   Users_Table : constant int := 1;   -- (id, username, created_at)
   Likes_Index : constant int := 2;   -- likes_post_id_idx: (post_id, rowid)
   Newest_Index : constant int := 3;  -- posts_created_at_id_idx: (created_at DESC, id DESC, rowid)
   Fast_Reads : Boolean := False;
   Fast_Served, SQL_Fallbacks : Natural := 0;   -- since the last stats line

   type Field_Kind is (Null_Field, Int_Field, Text_Field, Other_Field);
   type Field is record
      Kind : Field_Kind := Other_Field;
      Int : Long_Long_Integer := 0;
      Text : System.Address := System.Null_Address;
      Length : Natural := 0;
   end record;
   type Field_List is array (1 .. 4) of Field;
   type Byte_Array is array (Natural range <>) of Unsigned_8;
   function To_Signed is new Ada.Unchecked_Conversion (Unsigned_64, Long_Long_Integer);

   -- Decodes the first Count fields of the current record of Cursor (SQLite
   -- record format: a header of varint serial types, then the values).
   -- False if there is no whole record or it has fewer or unknown fields.
   function Decode (Cursor : int; Count : Positive; Result : out Field_List) return Boolean is
      Size : aliased unsigned := 0;
      Data : constant System.Address := BT_Record (Cursor, Size'Access);
   begin
      if Data = System.Null_Address or else Size = 0 or else Size > 65_536 then
         return False;
      end if;
      declare
         use System.Storage_Elements;
         Bytes : Byte_Array (0 .. Natural (Size) - 1) with Import, Address => Data;
         Pos : Natural := 0;
         Header_End, Value_At, Width : Natural;
         Serial, V : Unsigned_64;

         function Varint return Unsigned_64 is
            Value : Unsigned_64 := 0;
            B : Unsigned_8;
         begin
            for I in 1 .. 8 loop
               B := Bytes (Pos);
               Pos := Pos + 1;
               Value := Shift_Left (Value, 7) or Unsigned_64 (B and 127);
               if B < 128 then
                  return Value;
               end if;
            end loop;
            B := Bytes (Pos);
            Pos := Pos + 1;
            return Shift_Left (Value, 8) or Unsigned_64 (B);
         end Varint;
      begin
         V := Varint;
         if V > Unsigned_64 (Size) then
            return False;
         end if;
         Header_End := Natural (V);
         Value_At := Header_End;
         for F in 1 .. Count loop
            if Pos >= Header_End then
               return False;
            end if;
            Serial := Varint;
            Result (F) := (others => <>);
            case Serial is
               when 0 =>
                  Result (F).Kind := Null_Field;
                  Width := 0;
               when 1 .. 6 =>
                  Width := (case Serial is when 5 => 6, when 6 => 8, when others => Natural (Serial));
                  if Value_At + Width > Natural (Size) then
                     return False;
                  end if;
                  V := 0;
                  for I in 0 .. Width - 1 loop
                     V := Shift_Left (V, 8) or Unsigned_64 (Bytes (Value_At + I));
                  end loop;
                  if Width < 8 and then (V and Shift_Left (1, Width * 8 - 1)) /= 0 then
                     V := V or Shift_Left (Unsigned_64'Last, Width * 8);  -- negative
                  end if;
                  Result (F).Kind := Int_Field;
                  Result (F).Int := To_Signed (V);
               when 8 | 9 =>
                  Result (F).Kind := Int_Field;
                  Result (F).Int := Long_Long_Integer (Serial - 8);
                  Width := 0;
               when 10 | 11 =>
                  return False;
               when others =>
                  if Serial > 2 * 65_536 + 13 then
                     return False;
                  end if;
                  Width := Natural ((Serial - 12) / 2);
                  if Serial mod 2 = 1 then
                     Result (F).Kind := Text_Field;
                     Result (F).Text := Data + Storage_Offset (Value_At);
                     Result (F).Length := Width;
                  end if;
            end case;
            if Value_At + Width > Natural (Size) then
               return False;
            end if;
            Value_At := Value_At + Width;
         end loop;
         return True;
      end;
   exception
      when Constraint_Error =>
         return False;
   end Decode;

   procedure Put_Field (F : Field) is
   begin
      if F.Length = 0 then
         Output.Put ("""""");
      else
         Output.Put_Quoted (F.Text, F.Length);
      end if;
   end Put_Field;

   -- Likes of one post, from likes_post_id_idx; -1 if it cannot tell.
   function Fast_Like_Count (Post_ID : Long_Long_Integer) return Long_Long_Integer is
      Key : Field_List;
      RC : int := BT_Seek_Int (Likes_Index, Post_ID);
      Count : Long_Long_Integer := 0;
   begin
      while RC = 1 loop
         if not Decode (Likes_Index, 1, Key) or else Key (1).Kind /= Int_Field then
            return -1;
         end if;
         exit when Key (1).Int /= Post_ID;
         Count := Count + 1;
         RC := BT_Step (Likes_Index, 0);
      end loop;
      return (if RC < 0 then -1 else Count);
   end Fast_Like_Count;

   -- The current posts row as {"id":..,"body":..,"created_at":..,"author":..,
   -- "like_count":..}, its author read from users. False (nothing written
   -- that the caller keeps) if the row or its author is not as expected.
   function Fast_Post_JSON (ID, Like_Count : Long_Long_Integer) return Boolean is
      Post_Row, User_Row : Field_List;
   begin
      if not Decode (Posts_Table, 4, Post_Row) or else Post_Row (2).Kind /= Int_Field
        or else Post_Row (3).Kind /= Text_Field or else Post_Row (4).Kind /= Text_Field
        or else BT_Seek_Row (Users_Table, Post_Row (2).Int) /= 1
        or else not Decode (Users_Table, 2, User_Row) or else User_Row (2).Kind /= Text_Field
      then
         return False;
      end if;
      Output.Put ("{""id"":");
      Output.Put_Int (ID);
      Output.Put (",""body"":");
      Put_Field (Post_Row (3));
      Output.Put (",""created_at"":");
      Put_Field (Post_Row (4));
      Output.Put (",""author"":");
      Put_Field (User_Row (2));
      Output.Put (",""like_count"":");
      Output.Put_Int (Like_Count);
      Output.Put ('}');
      return True;
   end Fast_Post_JSON;

   type Fast_Result is (Served, Not_Found, Use_SQL);

   function Fast_Post (ID : Long_Long_Integer) return Fast_Result is
      Start : constant Natural := Output.Mark;
      Result : Fast_Result := Use_SQL;
      Found, Likes : Long_Long_Integer;
   begin
      if BT_Begin /= 0 then
         return Use_SQL;
      end if;
      begin
         Found := Long_Long_Integer (BT_Seek_Row (Posts_Table, ID));
         if Found = 0 then
            Result := Not_Found;
         elsif Found = 1 then
            Likes := Fast_Like_Count (ID);
            if Likes >= 0 then
               Output.Put ("{""post"":");
               if Fast_Post_JSON (ID, Likes) then
                  Output.Put ('}');
                  Result := Served;
               end if;
            end if;
         end if;
      exception
         when others =>   -- e.g. a response too large: the SQL path reports it
            Result := Use_SQL;
      end;
      BT_End;
      if Result = Use_SQL then
         Output.Rewind (Start);
         SQL_Fallbacks := SQL_Fallbacks + 1;
      else
         Fast_Served := Fast_Served + 1;
      end if;
      return Result;
   end Fast_Post;

   function Fast_Feed return Boolean is
      Span : constant := 40;
      IDs : array (1 .. 20) of Long_Long_Integer := [others => 0];
      Likes : array (0 .. Span - 1) of Long_Long_Integer := [others => 0];
      Count, Next : Natural := 0;
      Newest, Oldest, Row : Long_Long_Integer;
      Key : Field_List;
      RC : int;
      Start : constant Natural := Output.Mark;
      OK : Boolean := False;
   begin
      if BT_Begin /= 0 then
         return False;
      end if;
      begin
         -- The newest 20 posts, in posts_created_at_id_idx order.
         RC := BT_First (Newest_Index);
         while RC = 1 and then Count < IDs'Last loop
            exit when not Decode (Newest_Index, 2, Key) or else Key (2).Kind /= Int_Field;
            Count := Count + 1;
            IDs (Count) := Key (2).Int;
            RC := BT_Step (Newest_Index, 0);
         end loop;
         if Count = IDs'Last or else (RC = 0 and then Count > 0) then
            Newest := IDs (1);
            Oldest := IDs (Count);
            -- As in Feed: dense, newest-first IDs only.
            OK := Oldest <= Long_Long_Integer'Last - Span and then Newest < Oldest + Span;
            for I in 2 .. Count loop
               OK := OK and then IDs (I) < IDs (I - 1);
            end loop;
         elsif RC = 0 and then Count = 0 then
            Output.Put ("{""posts"":[]}");
            BT_End;
            Fast_Served := Fast_Served + 1;
            return True;
         end if;
         if OK then
            -- Their likes: one range of likes_post_id_idx.
            RC := BT_Seek_Int (Likes_Index, Oldest);
            while RC = 1 loop
               if not Decode (Likes_Index, 1, Key) or else Key (1).Kind /= Int_Field then
                  OK := False;
                  exit;
               end if;
               exit when Key (1).Int > Newest;
               Likes (Natural (Key (1).Int - Oldest)) := Likes (Natural (Key (1).Int - Oldest)) + 1;
               RC := BT_Step (Likes_Index, 0);
            end loop;
            OK := OK and then RC >= 0;
         end if;
         if OK then
            -- The rows, newest first, walking posts backwards from Newest.
            Output.Put ("{""posts"":[");
            OK := BT_Seek_Row (Posts_Table, Newest) = 1;
            Next := 1;
            while OK and then Next <= Count loop
               Row := BT_Row_ID (Posts_Table);
               if Row = IDs (Next) then
                  if Next > 1 then
                     Output.Put (',');
                  end if;
                  OK := Fast_Post_JSON (Row, Likes (Natural (Row - Oldest)));
                  Next := Next + 1;
               elsif Row < IDs (Next) then
                  OK := False;   -- a wanted post is missing
               end if;
               if OK and then Next <= Count then
                  OK := BT_Step (Posts_Table, 1) = 1;
               end if;
            end loop;
            if OK then
               Output.Put ("]}");
            end if;
         end if;
      exception
         when others =>
            OK := False;
      end;
      BT_End;
      if OK then
         Fast_Served := Fast_Served + 1;
      else
         Output.Rewind (Start);
         SQL_Fallbacks := SQL_Fallbacks + 1;
      end if;
      return OK;
   end Fast_Feed;

   -- Enables Fast_Reads when the schema is exactly the challenge's.
   procedure Setup_Fast_Reads is
      function Scalar (SQL : String) return String is
         S : aliased Handle := System.Null_Address;
         Ignored : int;
      begin
         Require (Prepare (DB, To_C (SQL), -1, 0, S'Access, System.Null_Address));
         declare
            Value : constant String := (if Step (S) = Row then Column_Text (S, 0) else "");
         begin
            Ignored := Finalize (S);
            return Value;
         end;
      end Scalar;
      function Root (Name : String) return unsigned is
         Text : constant String :=
           Scalar ("SELECT rootpage FROM sqlite_schema WHERE name='" & Name & "'");
      begin
         return (if Text = "" then 0 else unsigned'Value (Text));
      end Root;
      function Columns (Table : String) return String is
        (Scalar ("SELECT group_concat(name||':'||upper(type)||':'||pk,',') FROM " &
                 "(SELECT * FROM pragma_table_info('" & Table & "') ORDER BY cid)"));
      function Key (Index : String) return String is
        (Scalar ("SELECT group_concat(coalesce(name,'rowid')||':'||""desc"",',') FROM " &
                 "(SELECT * FROM pragma_index_xinfo('" & Index & "') WHERE key ORDER BY seqno)"));
      Newest_Key : constant String := Key ("posts_created_at_id_idx");
   begin
      Fast_Reads := False;
      if Columns ("posts") /= "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0"
        or else Columns ("users") /= "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0"
        or else Key ("likes_post_id_idx") /= "post_id:0"
        or else (Newest_Key /= "created_at:1,id:1" and then Newest_Key /= "created_at:1,rowid:1")
        or else Scalar ("PRAGMA encoding") /= "UTF-8"
      then
         return;
      end if;
      Fast_Reads :=
        BT_Setup (DB, Posts_Table, Root ("posts"), 0, 0) = 0 and then
        BT_Setup (DB, Users_Table, Root ("users"), 0, 0) = 0 and then
        BT_Setup (DB, Likes_Index, Root ("likes_post_id_idx"), 1, 0) = 0 and then
        BT_Setup (DB, Newest_Index, Root ("posts_created_at_id_idx"), 2, 1) = 0;
   end Setup_Fast_Reads;

   procedure Open (Path : String) is
      Connection : aliased Handle := System.Null_Address;
      S : aliased Handle := System.Null_Address;
      Ignored : int;
      Ignored_Address : System.Address;
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
           "PRAGMA cache_size=-32768; PRAGMA mmap_size=0;" &
           "PRAGMA temp_store=MEMORY;");
      -- Replaces SQLite's automatic checkpoint (see On_Commit).
      Ignored_Address := Set_WAL_Hook (DB, On_Commit'Address, System.Null_Address);
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
      Setup_Fast_Reads;
   end Open;

   procedure Close is
      Ignored : int;
   begin
      Fast_Reads := False;
      BT_Close;
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

   function Writes_Held return Boolean is (Holding);

   function Release_Writes return Boolean is
   begin
      if Holding then
         declare
            Held_For : constant Unsigned_64 := Sys.Monotonic_Milliseconds - Gate_Start;
         begin
            if Checkpointer.Caught_Up (Ticket) then
               Gates := Gates + 1;
            elsif Held_For >= Gate_Timeout then
               Gate_Timeouts := Gate_Timeouts + 1;
            else
               return False;
            end if;
            Worst_Gate := Unsigned_64'Max (Worst_Gate, Held_For);
         end;
         Holding := False;
         return True;
      end if;
      return False;
   end Release_Writes;

   function Checkpoint_Stats return String is
      Text : constant String := " gates" & Gates'Image & " gate_timeouts" & Gate_Timeouts'Image &
        " worst_gate_ms" & Worst_Gate'Image & " inline_ckpt" & Inline_Checkpoints'Image &
        (if Fast_Reads then " btree_reads" & Fast_Served'Image & " sql_fallbacks" & SQL_Fallbacks'Image
         else " btree_reads off");
   begin
      Fast_Served := 0;
      SQL_Fallbacks := 0;
      Worst_Gate := 0;
      Gates := 0;
      Gate_Timeouts := 0;
      Inline_Checkpoints := 0;
      return Text;
   end Checkpoint_Stats;

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
      if Fast_Reads and then Fast_Feed then
         return;
      end if;
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
      if Fast_Reads then
         case Fast_Post (ID) is
            when Served => return True;
            when Not_Found => return False;
            when Use_SQL => null;
         end case;
      end if;
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
