with Ada.Text_IO;
with Interfaces.C; use Interfaces.C;
with System;
with Sys;

package body Checkpointer is
   use Interfaces;
   use type System.Address;

   -- A routine pass every Interval keeps the work per pass small; the
   -- server's requests are noticed within Poll.
   Interval : constant := 250;  -- ms
   Poll : constant Duration := 0.005;

   -- One page shared by both processes (MAP_SHARED | MAP_ANONYMOUS, mapped
   -- before fork). Each slot is written by one side only.
   Requested : constant := 0;     -- server: ticket of the latest request
   Started_For : constant := 1;   -- helper: Requested as read when a pass began
   Finished_For : constant := 2;  -- helper: Started_For once that pass ended
   Frames_Left : constant := 3;   -- helper: WAL frames not yet copied, as of that pass
   Heartbeat : constant := 4;     -- helper: bumped every Poll

   Shared : System.Address := System.Null_Address;
   Helper : Boolean := False;
   Last_Beat : Unsigned_32 := 0;
   Last_Beat_Change : Unsigned_64 := 0;

   function Load_32 (Ptr : System.Address; Model : Integer) return Unsigned_32
     with Import, Convention => Intrinsic, External_Name => "__atomic_load_4";
   procedure Store_32 (Ptr : System.Address; Value : Unsigned_32; Model : Integer)
     with Import, Convention => Intrinsic, External_Name => "__atomic_store_4";
   Relaxed : constant := 0;
   Acquire : constant := 2;
   Release : constant := 3;

   function Slot (Index : Natural) return System.Address is (Sys.Offset (Shared, Index * 4));

   function Fork return int
     with Import, Convention => C, External_Name => "fork";
   function Process_ID return int
     with Import, Convention => C, External_Name => "getpid";
   function Parent_ID return int
     with Import, Convention => C, External_Name => "getppid";
   procedure Quit (Status : int)
     with Import, Convention => C, External_Name => "_exit", No_Return;
   function Set_Priority (Which, Who, Priority : int) return int
     with Import, Convention => C, External_Name => "setpriority";

   function Open_File (Path : char_array; Flags : int) return int
     with Import, Convention => C, External_Name => "open";
   function Read_At (FD : int; Buffer : System.Address; Count : size_t; Offset : long) return long
     with Import, Convention => C, External_Name => "pread";
   function Close_File (FD : int) return int
     with Import, Convention => C, External_Name => "close";

   function Open (Name : char_array; Result : access System.Address;
                  Flags : int; VFS : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_open_v2";
   function Close (Connection : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_close_v2";
   function Exec (Connection : System.Address; SQL : char_array;
                  Callback, Arg, Error : System.Address) return int
     with Import, Convention => C, External_Name => "sqlite3_exec";
   function Checkpoint (Connection : System.Address; Name : System.Address; Mode : int;
                        Log, Done : access int) return int
     with Import, Convention => C, External_Name => "sqlite3_wal_checkpoint_v2";

   -- True once the server has put the file in WAL mode: header bytes 18
   -- and 19 (file format read/write versions) are 2. Reading the header
   -- directly takes no SQLite lock while the server converts the file.
   function In_WAL (Path : String) return Boolean is
      Header : array (0 .. 19) of unsigned_char := [others => 0];
      FD : constant int := Open_File (To_C (Path), 0);  -- O_RDONLY
      Got : long;
      Ignored : int;
   begin
      if FD < 0 then
         return False;
      end if;
      Got := Read_At (FD, Header'Address, 20, 0);
      Ignored := Close_File (FD);
      return Got = 20 and then Header (18) = 2 and then Header (19) = 2;
   end In_WAL;

   procedure Run (Path : String; Parent : int) is
      Connection : aliased System.Address := System.Null_Address;
      Log, Done : aliased int := 0;
      Beat : Unsigned_32 := 0;
      Ticket : Unsigned_32;
      Last_Ticket : Unsigned_32 := 0;
      Last_Pass : Unsigned_64 := 0;
      Ignored : int;
   begin
      -- Yield the CPU to the event loop whenever both want it.
      Ignored := Set_Priority (0, 0, 10);
      loop
         if Sys.Stop_Requested or else Parent_ID /= Parent then
            if Connection /= System.Null_Address then
               Ignored := Close (Connection);
            end if;
            Quit (0);
         end if;
         Beat := Beat + 1;
         Store_32 (Slot (Heartbeat), Beat, Relaxed);
         if Connection = System.Null_Address then
            if In_WAL (Path) then
               -- One read makes this connection open the WAL; until then a
               -- checkpoint call does nothing.
               -- SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX
               if Open (To_C (Path), Connection'Access, 2 + 32768, System.Null_Address) /= 0
                 or else Exec (Connection, To_C ("SELECT 1 FROM sqlite_master LIMIT 1"),
                               System.Null_Address, System.Null_Address,
                               System.Null_Address) /= 0
               then
                  Ignored := Close (Connection);
                  Connection := System.Null_Address;
               end if;
            end if;
         else
            Ticket := Load_32 (Slot (Requested), Acquire);
            if Ticket /= Last_Ticket or else Sys.Monotonic_Milliseconds - Last_Pass >= Interval then
               Store_32 (Slot (Started_For), Ticket, Relaxed);
               -- PASSIVE never waits for readers or writers and never takes
               -- the write lock, so the server is never blocked by it.
               Ignored := Checkpoint (Connection, System.Null_Address, 0, Log'Access, Done'Access);
               if Log < 0 then
                  -- The connection is not using the WAL after all: start over.
                  Ignored := Close (Connection);
                  Connection := System.Null_Address;
               else
                  Store_32 (Slot (Frames_Left),
                            (if Ignored = 0 then Unsigned_32 (Log - Done) else 16#FFFF_FFFF#),
                            Relaxed);
                  Store_32 (Slot (Finished_For), Ticket, Release);
               end if;
               Last_Ticket := Ticket;
               Last_Pass := Sys.Monotonic_Milliseconds;
            end if;
         end if;
         delay Poll;
      end loop;
   end Run;

   procedure Start (Path : String) is
      Parent : constant int := Process_ID;
      Child : int;
   begin
      -- PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS: zero-filled.
      Shared := Sys.Map (System.Null_Address, 4096, 3, 1 + 16#20#, -1, 0);
      if Shared = Sys.Map_Failed then
         Shared := System.Null_Address;
         Ada.Text_IO.Put_Line ("checkpoint helper unavailable; checkpointing inline");
         return;
      end if;
      -- Buffered output would otherwise be written by both processes.
      Ada.Text_IO.Flush (Ada.Text_IO.Standard_Output);
      Ada.Text_IO.Flush (Ada.Text_IO.Standard_Error);
      Child := Fork;
      if Child = 0 then
         begin
            Run (Path, Parent);
         exception
            when others =>
               Quit (1);
         end;
      elsif Child < 0 then
         -- The server still works without the helper: it then checkpoints
         -- inline (see Database).
         Ada.Text_IO.Put_Line ("checkpoint helper unavailable; checkpointing inline");
      else
         Helper := True;
         Last_Beat_Change := Sys.Monotonic_Milliseconds;
      end if;
   end Start;

   function Available return Boolean is
      Beat : Unsigned_32;
      Now : constant Unsigned_64 := Sys.Monotonic_Milliseconds;
   begin
      if not Helper then
         return False;
      end if;
      Beat := Load_32 (Slot (Heartbeat), Relaxed);
      if Beat /= Last_Beat then
         Last_Beat := Beat;
         Last_Beat_Change := Now;
      end if;
      -- At nice 10 the helper still runs within milliseconds; two seconds
      -- without a beat means it is gone or stuck.
      return Now - Last_Beat_Change < 2_000;
   end Available;

   function Request_Pass return Unsigned_32 is
      Ticket : Unsigned_32;
   begin
      if not Helper then
         return 0;
      end if;
      Ticket := Load_32 (Slot (Requested), Relaxed) + 1;
      Store_32 (Slot (Requested), Ticket, Release);
      return Ticket;
   end Request_Pass;

   function Caught_Up (Ticket : Unsigned_32) return Boolean is
      Finished : Unsigned_32;
   begin
      if not Helper then
         return False;
      end if;
      Finished := Load_32 (Slot (Finished_For), Acquire);
      -- Finished is at or past Ticket (tickets wrap around).
      return Finished - Ticket < 16#8000_0000#
        and then Load_32 (Slot (Frames_Left), Relaxed) = 0;
   end Caught_Up;
end Checkpointer;
