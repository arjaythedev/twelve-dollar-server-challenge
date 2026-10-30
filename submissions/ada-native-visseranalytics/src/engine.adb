with Ada.Text_IO;
with Interfaces; use Interfaces;
with Interfaces.C; use Interfaces.C;
with System;
with System.Storage_Elements;
with Application;
with Database;
with HTTP;
with Output;
with Platform;
with Pool;
with Sys;
with Uring;

package body Engine is
   use type System.Address;
   use type HTTP.Method_Kind;

   -- Connection IDs: ring * Stride + slot with io_uring, the descriptor
   -- number with epoll.
   Stride : constant := 65_536;
   Max_Rings : constant := 6;
   Max_IDs : constant := Max_Rings * Stride;
   subtype Conn_ID is Natural range 0 .. Max_IDs - 1;

   Idle_Seconds : constant := 75;     -- idle keep-alive lifetime
   Request_Seconds : constant := 30;  -- unfinished request or response
   Pause_Bytes : constant := 65_536;  -- stop receiving past this queued input
   Input_Limit : constant := 1_048_576;

   -- Batching: when busy, wait for a few completions or Batch_Nanos so one
   -- wake-up serves many requests (and one transaction covers them).
   Busy_Completions : constant := 8;
   Wait_Target : constant := 64;
   Batch_Nanos : constant := 2_000_000;
   Idle_Nanos : constant := 2_000_000;

   type Conn is record
      Gen : Unsigned_32;
      In_Length : Unsigned_32;
      Out_Offset : Unsigned_32;
      Last_Active : Unsigned_32;
      Request_Start : Unsigned_32;
      Interest : Unsigned_32;
      Input : System.Address;      -- queued request bytes (pool block)
      Out_Block : System.Address;  -- response being sent (pool block)
      Open : Boolean;
      Writing : Boolean;           -- a response is queued or in flight
      Close_After : Boolean;
      Interim : Boolean;           -- the response is a 100 Continue
      Continue_Sent : Boolean;
      Receiving : Boolean;         -- multishot receive armed
      Paused : Boolean;            -- receive cancelled for backpressure
      Deferred : Boolean;          -- waiting for the batch commit
      In_Flight : Boolean;         -- send submitted to the kernel
      Queued : Boolean;            -- on the end-of-batch work list
      Held : Boolean;              -- a write request waits for the WAL restart
   end record;

   -- Untouched pages of an anonymous mapping cost no memory, so the table
   -- is sized for every possible connection and needs no initialization.
   type Conn_Table is array (Conn_ID) of Conn;
   function Map_Table return System.Address is
     (Sys.Map (System.Null_Address, size_t (Max_IDs) * size_t (Conn_Table'Component_Size / 8), 3,
               2 + 16#20# + 16#4000#, -1, 0));
   Table_Address : constant System.Address := Map_Table;
   Table : Conn_Table with Import, Address => Table_Address;

   type Backend is (Use_Uring, Use_Epoll);
   Mode : Backend := Use_Epoll;
   Now : Unsigned_32 := 0;

   type Deferred_Item is record
      ID : Conn_ID;
      Gen : Unsigned_32;
      Block : System.Address;
   end record;
   Max_Deferred : constant := 16_384;
   Deferred_List : array (1 .. Max_Deferred) of Deferred_Item;
   Deferred_Count : Natural := 0;

   -- Connections that need a receive re-armed (io_uring) or a send
   -- attempted (epoll) once the batch has committed.
   Work : array (1 .. Max_IDs) of Conn_ID;
   Work_Count : Natural := 0;

   -- Write requests waiting for a WAL restart (Database.Writes_Held). If the
   -- list is full, further writes simply run.
   type Held_Item is record
      ID : Conn_ID;
      Gen : Unsigned_32;
   end record;
   Max_Held : constant := 16_384;
   Held_List : array (1 .. Max_Held) of Held_Item;
   Held_Count : Natural := 0;

   -- io_uring releases closed sockets in bursts, and every FIN pins a page
   -- of socket memory until it is acknowledged. Closing at most a bounded
   -- number per loop turn keeps a mass disconnect from spiking memory.
   Close_Rate : constant := 200;      -- closes per millisecond
   Close_Burst : constant := 512;
   Close_Tokens : Natural := Close_Burst;
   Close_Clock : Unsigned_64 := 0;
   Closing : array (0 .. Max_IDs - 1) of Conn_ID;
   Closing_First, Closing_Count : Natural := 0;

   -- io_uring state.
   Tag_Accept : constant := 1;
   Tag_Receive : constant := 2;
   Tag_Send : constant := 3;
   Tag_Close : constant := 4;
   Tag_Ignore : constant := 5;
   -- Requests are small (a GET is ~100 bytes, a new post with its token
   -- under 1 KB); a larger one simply arrives in several buffers.
   Buffer_Count : constant := 2_048;
   Buffer_Size : constant := 1_024;
   Rings : array (0 .. Max_Rings - 1) of Uring.Ring;
   Ring_Count : Natural := 0;
   Slots : Natural := 0;
   Margin : Natural := 0;
   Open_Count : array (0 .. Max_Rings - 1) of Natural := [others => 0];
   Highest : array (0 .. Max_Rings - 1) of Natural := [others => 0];
   Accept_Ring : Integer := -1;
   Accept_Cancelling : Boolean := False;
   Accept_Retry : Unsigned_32 := 0;     -- after a hard accept error
   Listen_FD : Integer := -1;

   -- Diagnostics, printed every 10 seconds.
   Stat_Batches, Stat_Completions, Stat_No_Buffers : Natural := 0;
   Stat_Worst_Batch, Stat_Worst_Commit, Stat_Worst_Sweep : Unsigned_64 := 0;
   Stat_Clock : Unsigned_32 := 0;
   -- Loop turns of Slow_Loop_Ms or more, and where the worst one spent its
   -- time (submit, wait, events, drain, flush, SQL commit; milliseconds).
   Slow_Loop_Ms : constant := 50;
   Last_SQL_Ms : Unsigned_64 := 0;
   Stat_Slow_Loops : Natural := 0;
   Stat_Worst_Loop : Unsigned_64 := 0;
   Stat_Worst_Phases : array (1 .. 6) of Unsigned_64 := [others => 0];

   -- epoll state.
   -- struct epoll_event is 12 bytes on x86_64 and 16 on AArch64. Its data
   -- carries the descriptor AND generation so a stale event cannot reach a
   -- reused descriptor.
   type Ready_Event is record
      Events : Unsigned_32 := 0;
      Slot : Unsigned_32 := 0;
      Generation : Unsigned_32 := 0;
   end record with Convention => C;
   for Ready_Event use record
      Events at 0 range 0 .. 31;
      Slot at Platform.Epoll_Data_Offset range 0 .. 31;
      Generation at Platform.Epoll_Data_Offset + 4 range 0 .. 31;
   end record;
   for Ready_Event'Size use Platform.Epoll_Event_Bytes * 8;
   for Ready_Event'Alignment use Platform.Epoll_Event_Alignment;
   Queue : int := -1;
   Highest_FD : Natural := 0;
   Readable : constant := 1;
   Writable : constant := 4;
   Broken : constant := 8 + 16;

   function To_U64 (Address : System.Address) return Unsigned_64 is
     (Unsigned_64 (System.Storage_Elements.To_Integer (Address)));
   function To_Address (Value : Unsigned_64) return System.Address is
     (System.Storage_Elements.To_Address (System.Storage_Elements.Integer_Address (Value)));

   function Receive_Data (ID : Conn_ID; Gen : Unsigned_32) return Unsigned_64 is
     (Tag_Receive + Shift_Left (Unsigned_64 (Gen and 16#0FFF_FFFF#), 4) +
      Shift_Left (Unsigned_64 (ID), 32));
   function Accept_Data (R : Natural) return Unsigned_64 is
     (Tag_Accept + Shift_Left (Unsigned_64 (R), 8));

   procedure Fail (Message : String) with No_Return is
   begin
      raise Program_Error with Message;
   end Fail;

   function Next_Entry (R : Natural) return System.Address is
      E : System.Address := Uring.Get (Rings (R));
      RC : Integer;
   begin
      while E = System.Null_Address loop
         -- Full submission queue: hand it to the kernel and retry.
         RC := Uring.Enter (Rings (R), Get_Events => False);
         if RC = -Sys.EBUSY then
            RC := Uring.Enter (Rings (R), Get_Events => True);
         end if;
         if RC < 0 and then RC /= -Sys.EINTR and then RC /= -Sys.EAGAIN and then RC /= -Sys.EBUSY then
            Fail ("io_uring_enter failed:" & RC'Image);
         end if;
         E := Uring.Get (Rings (R));
      end loop;
      return E;
   end Next_Entry;

   ---------------------------------------------------------------------------
   -- Shared connection logic
   ---------------------------------------------------------------------------

   procedure Queue_Work (ID : Conn_ID) is
      C : Conn renames Table (ID);
   begin
      if not C.Queued then
         C.Queued := True;
         Work_Count := Work_Count + 1;
         Work (Work_Count) := ID;
      end if;
   end Queue_Work;

   procedure Set_Interest (ID : Conn_ID; Want : Unsigned_32) is
      C : Conn renames Table (ID);
      Change : aliased Ready_Event := (Want, Unsigned_32 (ID), C.Gen);
   begin
      if C.Interest /= Want then
         if Sys.Epoll_Control (Queue, 3, int (ID), Change'Address) = 0 then
            C.Interest := Want;
         end if;
      end if;
   end Set_Interest;

   procedure Close_Conn (ID : Conn_ID) is
      C : Conn renames Table (ID);
      Ignored : int;
   begin
      if not C.Open then
         return;
      end if;
      C.Open := False;
      if C.Input /= System.Null_Address then
         Pool.Give (C.Input);
         C.Input := System.Null_Address;
         C.In_Length := 0;
      end if;
      -- A deferred block belongs to the deferred list and an in-flight one
      -- to its pending completion; both see the closed connection later.
      if C.Out_Block /= System.Null_Address and then not C.Deferred
        and then not C.In_Flight
      then
         Pool.Give (C.Out_Block);
      end if;
      C.Out_Block := System.Null_Address;
      C.Writing := False;
      if Mode = Use_Uring then
         -- The slot stays reserved until its close completes.
         Closing ((Closing_First + Closing_Count) mod Max_IDs) := ID;
         Closing_Count := Closing_Count + 1;
      else
         Ignored := Sys.Close (int (ID));
      end if;
   end Close_Conn;

   procedure Release_Closed is
      ID : Conn_ID;
      R : Natural;
      E : System.Address;
      Clock : Unsigned_64;
   begin
      if Closing_Count = 0 then
         return;
      end if;
      Clock := Sys.Monotonic_Milliseconds;
      if Clock > Close_Clock then
         Close_Tokens := Natural (Unsigned_64'Min (Close_Burst,
           Unsigned_64 (Close_Tokens) + (Clock - Close_Clock) * Close_Rate));
         Close_Clock := Clock;
      end if;
      for N in 1 .. Natural'Min (Close_Tokens, Closing_Count) loop
         Close_Tokens := Close_Tokens - 1;
         ID := Closing (Closing_First);
         Closing_First := (Closing_First + 1) mod Max_IDs;
         Closing_Count := Closing_Count - 1;
         R := ID / Stride;
         -- A request still holding the socket keeps it open after the slot
         -- is closed, so cancel it first, but cheaply: cancelling by slot
         -- makes the kernel scan every pending request on the ring (tens of
         -- thousands), while cancelling by user data is a hash lookup. When
         -- the peer closed first, the receive has already ended and there
         -- is nothing to cancel.
         declare
            C : Conn renames Table (ID);
         begin
            if C.In_Flight then
               -- A send may be pending as well: cancel everything (rare).
               E := Next_Entry (R);
               Uring.Prepare_Cancel_Slot (E, ID mod Stride, Tag_Ignore);
            elsif C.Receiving then
               E := Next_Entry (R);
               Uring.Prepare_Cancel (E, Receive_Data (ID, C.Gen), Tag_Ignore);
            end if;
         end;
         E := Next_Entry (R);
         Uring.Prepare_Close (E, ID mod Stride, Tag_Close + Shift_Left (Unsigned_64 (R), 8));
      end loop;
   end Release_Closed;

   procedure Open_Conn (ID : Conn_ID) is
      C : Conn renames Table (ID);
   begin
      -- Queued survives: a stale work-list entry then serves this connection.
      C := (Gen => C.Gen + 1, In_Length => 0, Out_Offset => 0,
            Last_Active => Now, Request_Start => Now, Interest => Readable,
            Input | Out_Block => System.Null_Address, Open => True,
            Queued => C.Queued,
            Writing | Close_After | Interim | Continue_Sent | Receiving |
            Paused | Deferred | In_Flight | Held => False);
   end Open_Conn;

   procedure Start_Send (ID : Conn_ID) is
      C : Conn renames Table (ID);
   begin
      if Mode = Use_Uring then
         declare
            E : constant System.Address := Next_Entry (ID / Stride);
         begin
            Uring.Prepare_Send
              (E, ID mod Stride, Sys.Offset (Pool.Data (C.Out_Block), Natural (C.Out_Offset)),
               Pool.Length (C.Out_Block) - Natural (C.Out_Offset),
               To_U64 (C.Out_Block) + Tag_Send);
            C.In_Flight := True;
         end;
      else
         Queue_Work (ID);
      end if;
   end Start_Send;

   procedure Flush_Deferred is
      SQL_Started : constant Unsigned_64 := Sys.Monotonic_Milliseconds;
      OK : constant Boolean := Database.Finish_Batch;
   begin
      Last_SQL_Ms := Sys.Monotonic_Milliseconds - SQL_Started;
      for I in 1 .. Deferred_Count loop
         declare
            D : Deferred_Item renames Deferred_List (I);
            C : Conn renames Table (D.ID);
         begin
            if C.Open and then C.Gen = D.Gen and then C.Out_Block = D.Block and then C.Deferred then
               C.Deferred := False;
               if not OK then
                  -- The batch did not commit: no success may be reported.
                  Pool.Give (D.Block);
                  Output.Start;
                  Application.Error_Body ("internal server error");
                  C.Out_Block := Output.Finish (500, C.Close_After);
                  Pool.Set_Owner (C.Out_Block, Unsigned_32 (D.ID), C.Gen);
                  C.Out_Offset := 0;
               end if;
               Start_Send (D.ID);
            else
               Pool.Give (D.Block);
            end if;
         end;
      end loop;
      Deferred_Count := 0;
   end Flush_Deferred;

   procedure Queue_Response (ID : Conn_ID; Block : System.Address; Close, Defer : Boolean) is
      C : Conn renames Table (ID);
   begin
      Pool.Set_Owner (Block, Unsigned_32 (ID), C.Gen);
      C.Writing := True;
      C.Out_Block := Block;
      C.Out_Offset := 0;
      C.Close_After := Close;
      C.Request_Start := Now;
      if Defer then
         C.Deferred := True;
         Deferred_Count := Deferred_Count + 1;
         Deferred_List (Deferred_Count) := (ID, C.Gen, Block);
         if Deferred_Count = Max_Deferred then
            Flush_Deferred;
         end if;
      else
         Start_Send (ID);
      end if;
   end Queue_Response;

   procedure Respond_Error (ID : Conn_ID; Status : Positive; Message : String) is
   begin
      Output.Start;
      Application.Error_Body (Message);
      Queue_Response (ID, Output.Finish (Status, True), Close => True, Defer => False);
   end Respond_Error;

   -- Handles at most one request: the next waits until its response is sent.
   procedure Process (ID : Conn_ID; Address : System.Address; Length : Natural;
                      Shared : Boolean) is
      C : Conn renames Table (ID);
      Data : String (1 .. Length) with Import, Address => Address;
      Item : HTTP.Request;
      Status : HTTP.Parse_Status;
      Keep : Natural := 0;
   begin
      HTTP.Parse (Data, Item, Status);
      case Status is
         when HTTP.Incomplete =>
            if Length >= HTTP.Max_Input then
               Respond_Error (ID, 413, "request too large");
            else
               Keep := Length;
               if Item.Expect_Continue and then not C.Continue_Sent then
                  C.Continue_Sent := True;
                  C.Interim := True;
                  Queue_Response (ID, Output.Raw ("HTTP/1.1 100 Continue" & ASCII.CR & ASCII.LF &
                                                  ASCII.CR & ASCII.LF),
                                  Close => False, Defer => False);
               end if;
            end if;
         when HTTP.Bad_Request =>
            Respond_Error (ID, 400, "bad request");
         when HTTP.Too_Large =>
            Respond_Error (ID, 413, "request too large");
         when HTTP.Complete =>
            if Item.Method = HTTP.Post and then Database.Writes_Held and then
              (C.Held or else Held_Count < Max_Held)
            then
               -- Kept whole until writes are released (Release_Held).
               if not C.Held then
                  C.Held := True;
                  Held_Count := Held_Count + 1;
                  Held_List (Held_Count) := (ID, C.Gen);
                  if not C.Writing then
                     C.Request_Start := Now;
                  end if;
               end if;
               Keep := Length;
            else
               declare
                  Code : constant Positive := Application.Handle (Data, Item);
                  Block : constant System.Address := Output.Finish (Code, Item.Close);
               begin
                  C.Continue_Sent := False;
                  C.Held := False;
                  Queue_Response (ID, Block, Item.Close, Defer => Database.In_Transaction);
               end;
               Keep := Length - Item.Consumed;
            end if;
      end case;
      if Shared then
         if Keep > 0 then
            C.Input := Pool.Take (Natural'Max (Keep, 4_096));
            Sys.Copy (Pool.Data (C.Input), Data (Length - Keep + 1)'Address, size_t (Keep));
            C.In_Length := Unsigned_32 (Keep);
            if not C.Writing then
               C.Request_Start := Now;
            end if;
         end if;
      elsif Keep = 0 then
         Pool.Give (C.Input);
         C.Input := System.Null_Address;
         C.In_Length := 0;
      elsif Keep < Length then
         Sys.Move (Pool.Data (C.Input), Data (Length - Keep + 1)'Address, size_t (Keep));
         C.In_Length := Unsigned_32 (Keep);
      end if;
   end Process;

   procedure Process_Buffered (ID : Conn_ID) is
      C : Conn renames Table (ID);
   begin
      Process (ID, Pool.Data (C.Input), Natural (C.In_Length), Shared => False);
   end Process_Buffered;

   function Append (ID : Conn_ID; Address : System.Address; Length : Natural) return Boolean is
      C : Conn renames Table (ID);
      Needed : constant Natural := Natural (C.In_Length) + Length;
      Larger : System.Address;
   begin
      if Needed > Input_Limit then
         return False;
      end if;
      if C.Input = System.Null_Address then
         C.Input := Pool.Take (Natural'Max (Needed, 4_096));
         C.In_Length := 0;
         if not C.Writing then
            C.Request_Start := Now;
         end if;
      elsif Needed > Pool.Capacity (C.Input) then
         Larger := Pool.Take (Needed);
         Sys.Copy (Pool.Data (Larger), Pool.Data (C.Input), size_t (C.In_Length));
         Pool.Give (C.Input);
         C.Input := Larger;
      end if;
      Sys.Copy (Sys.Offset (Pool.Data (C.Input), Natural (C.In_Length)), Address, size_t (Length));
      C.In_Length := Unsigned_32 (Needed);
      return True;
   end Append;

   procedure Pause (ID : Conn_ID) is
      C : Conn renames Table (ID);
      E : constant System.Address := Next_Entry (ID / Stride);
   begin
      C.Paused := True;
      Uring.Prepare_Cancel (E, Receive_Data (ID, C.Gen), Tag_Ignore);
   end Pause;

   procedure On_Data (ID : Conn_ID; Address : System.Address; Length : Natural) is
      C : Conn renames Table (ID);
   begin
      C.Last_Active := Now;
      if C.Writing or else C.In_Length > 0 then
         if not Append (ID, Address, Length) then
            Close_Conn (ID);
            return;
         end if;
         if not C.Writing then
            Process_Buffered (ID);
         elsif C.In_Length >= Pause_Bytes and then C.Receiving and then not C.Paused then
            Pause (ID);
         end if;
      else
         Process (ID, Address, Length, Shared => True);
      end if;
   end On_Data;

   procedure Complete_Send (ID : Conn_ID) is
      C : Conn renames Table (ID);
   begin
      Pool.Give (C.Out_Block);
      C.Out_Block := System.Null_Address;
      C.Writing := False;
      C.Interim := False;
      C.Last_Active := Now;
      if C.Close_After then
         Close_Conn (ID);
         return;
      end if;
      if Mode = Use_Uring then
         if C.In_Length > 0 then
            Process_Buffered (ID);
         end if;
         if C.Open and then C.Paused and then C.In_Length < Pause_Bytes / 2 then
            C.Paused := False;
            if not C.Receiving then
               Queue_Work (ID);
            end if;
         end if;
      else
         -- A queued pipelined request continues on the next loop turn.
         Set_Interest (ID, (if C.In_Length > 0 then Writable else Readable));
      end if;
   end Complete_Send;

   -- Runs the held write requests. Called before anything else in a batch,
   -- so the batch's transaction starts with them and the first one can
   -- restart the WAL.
   procedure Release_Held is
   begin
      for I in 1 .. Held_Count loop
         declare
            H : Held_Item renames Held_List (I);
            C : Conn renames Table (H.ID);
         begin
            if C.Open and then C.Gen = H.Gen and then C.Held then
               C.Held := False;
               if not C.Writing and then C.In_Length > 0 then
                  Process_Buffered (H.ID);
               end if;
            end if;
         end;
      end loop;
      Held_Count := 0;
   end Release_Held;

   procedure Check_Gate is
   begin
      if Database.Writes_Held then
         if Database.Release_Writes then
            Release_Held;
         end if;
      elsif Held_Count > 0 then
         Release_Held;
      end if;
   end Check_Gate;

   procedure Sweep is
      procedure Check (ID : Conn_ID) is
         C : Conn renames Table (ID);
      begin
         if C.Open and then
           ((not C.Writing and then C.In_Length = 0 and then Now - C.Last_Active >= Idle_Seconds)
            or else ((C.Writing or else C.In_Length > 0) and then
                     Now - C.Request_Start >= Request_Seconds))
         then
            Close_Conn (ID);
         end if;
      end Check;
   begin
      if Mode = Use_Uring then
         for R in 0 .. Ring_Count - 1 loop
            for S in 0 .. Highest (R) - 1 loop
               Check (R * Stride + S);
            end loop;
         end loop;
      else
         for FD in 0 .. Highest_FD loop
            Check (FD);
         end loop;
      end if;
   end Sweep;

   ---------------------------------------------------------------------------
   -- io_uring backend
   ---------------------------------------------------------------------------

   procedure Arm_Receive (ID : Conn_ID) is
      C : Conn renames Table (ID);
      E : constant System.Address := Next_Entry (ID / Stride);
   begin
      Uring.Prepare_Receive (E, ID mod Stride, Receive_Data (ID, C.Gen));
      C.Receiving := True;
   end Arm_Receive;

   procedure Try_Send (ID : Conn_ID);

   procedure End_Batch is
      I : Positive := 1;
      Started : constant Unsigned_64 := Sys.Monotonic_Milliseconds;
   begin
      Flush_Deferred;
      Stat_Worst_Commit := Unsigned_64'Max (Stat_Worst_Commit, Sys.Monotonic_Milliseconds - Started);
      if Mode = Use_Uring then
         for R in 0 .. Ring_Count - 1 loop
            Uring.Publish_Buffers (Rings (R));
         end loop;
         Release_Closed;
      end if;
      while I <= Work_Count loop
         declare
            ID : constant Conn_ID := Work (I);
            C : Conn renames Table (ID);
         begin
            C.Queued := False;
            if Mode = Use_Uring then
               if C.Open and then not C.Receiving and then not C.Paused then
                  Arm_Receive (ID);
               end if;
            else
               Try_Send (ID);
            end if;
         end;
         I := I + 1;
      end loop;
      Work_Count := 0;
   end End_Batch;

   procedure Keep_Accepting is
      E : System.Address;
   begin
      if Accept_Ring >= 0 then
         if not Accept_Cancelling and then Open_Count (Accept_Ring) + Margin >= Slots then
            -- Move on before this table fills: a direct accept into a full
            -- table would drop the connection.
            E := Next_Entry (Accept_Ring);
            Uring.Prepare_Cancel (E, Accept_Data (Accept_Ring), Tag_Ignore);
            Accept_Cancelling := True;
         end if;
         return;
      end if;
      if Now < Accept_Retry then
         return;
      end if;
      for R in 0 .. Ring_Count - 1 loop
         if Open_Count (R) + Margin < Slots then
            E := Next_Entry (R);
            Uring.Prepare_Accept (E, Listen_FD, Accept_Data (R));
            Accept_Ring := R;
            return;
         end if;
      end loop;
   end Keep_Accepting;

   procedure On_Accept (R : Natural; Result : Integer_32; Flags : Unsigned_32) is
      E : System.Address;
   begin
      if (Flags and Uring.F_More) = 0 and then Accept_Ring = R then
         Accept_Ring := -1;
         Accept_Cancelling := False;
      end if;
      if Result >= 0 then
         if Natural (Result) >= Slots then
            E := Next_Entry (R);
            Uring.Prepare_Close (E, Natural (Result), Tag_Ignore);
            return;
         end if;
         declare
            ID : constant Conn_ID := R * Stride + Natural (Result);
         begin
            Open_Count (R) := Open_Count (R) + 1;
            Highest (R) := Natural'Max (Highest (R), Natural (Result) + 1);
            Open_Conn (ID);
            Arm_Receive (ID);
         end;
      elsif (Flags and Uring.F_More) = 0 and then Result /= -Sys.ECANCELED
        and then Result /= -Sys.EINTR and then Result /= -103  -- ECONNABORTED
      then
         -- Do not spin on a persistent failure; retry in a second.
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "accept failed:" & Result'Image);
         Accept_Retry := Now + 1;
      end if;
   end On_Accept;

   procedure On_Receive (ID : Conn_ID; Gen : Unsigned_32; Result : Integer_32;
                         Flags : Unsigned_32) is
      C : Conn renames Table (ID);
      R : constant Natural := ID / Stride;
      Has_Buffer : constant Boolean := (Flags and Uring.F_Buffer) /= 0;
      Buffer_ID : constant Natural := Natural (Shift_Right (Flags, 16));
   begin
      if not C.Open or else (C.Gen and 16#0FFF_FFFF#) /= Gen then
         if Has_Buffer then
            Uring.Recycle (Rings (R), Buffer_ID);
         end if;
         return;
      end if;
      if (Flags and Uring.F_More) = 0 then
         C.Receiving := False;
      end if;
      if Result > 0 and then Has_Buffer then
         On_Data (ID, Uring.Buffer_Address (Rings (R), Buffer_ID), Natural (Result));
         Uring.Recycle (Rings (R), Buffer_ID);
      else
         if Has_Buffer then
            Uring.Recycle (Rings (R), Buffer_ID);
         end if;
         if Result = -Sys.ENOBUFS then
            Stat_No_Buffers := Stat_No_Buffers + 1;
         end if;
         if Result /= -Sys.ENOBUFS and then Result /= -Sys.ECANCELED
           and then Result /= -Sys.EINTR
         then
            -- End of stream or a socket error.
            Close_Conn (ID);
            return;
         end if;
      end if;
      if C.Open and then not C.Receiving and then not C.Paused then
         -- Re-arm after this batch has returned its buffers.
         Queue_Work (ID);
      end if;
   end On_Receive;

   procedure On_Send (Block : System.Address; Result : Integer_32) is
      Owner : constant Unsigned_32 := Pool.Owner (Block);
   begin
      if Owner >= Max_IDs then
         Pool.Give (Block);
         return;
      end if;
      declare
         ID : constant Conn_ID := Conn_ID (Owner);
         C : Conn renames Table (ID);
      begin
         if not C.Open or else C.Gen /= Pool.Gen (Block) or else C.Out_Block /= Block then
            Pool.Give (Block);
            return;
         end if;
         C.In_Flight := False;
         if Result <= 0 then
            C.Out_Block := System.Null_Address;
            C.Writing := False;
            Pool.Give (Block);
            Close_Conn (ID);
            return;
         end if;
         C.Out_Offset := C.Out_Offset + Unsigned_32 (Result);
         if Natural (C.Out_Offset) < Pool.Length (Block) then
            Start_Send (ID);
         else
            Complete_Send (ID);
         end if;
      end;
   end On_Send;

   procedure Dispatch (Item : Uring.Completion) is
   begin
      case Item.User_Data and 15 is
         when Tag_Receive =>
            On_Receive (Conn_ID (Shift_Right (Item.User_Data, 32)),
                        Unsigned_32 (Shift_Right (Item.User_Data, 4) and 16#0FFF_FFFF#),
                        Item.Result, Item.Flags);
         when Tag_Send =>
            On_Send (To_Address (Item.User_Data and not 15), Item.Result);
         when Tag_Accept =>
            On_Accept (Natural (Shift_Right (Item.User_Data, 8) and 255), Item.Result, Item.Flags);
         when Tag_Close =>
            declare
               Owner : constant Natural := Natural (Shift_Right (Item.User_Data, 8) and 255);
            begin
               Open_Count (Owner) := Open_Count (Owner) - 1;
            end;
         when others =>
            null;
      end case;
   end Dispatch;

   procedure Drain (R : Natural; Handled : in out Natural) is
      Item : Uring.Completion;
      Tail : Unsigned_32;
   begin
      loop
         Tail := Uring.Ready_Tail (Rings (R));
         exit when Rings (R).Head = Tail;
         while Rings (R).Head /= Tail loop
            Uring.Read (Rings (R), Rings (R).Head, Item);
            Uring.Advance (Rings (R), Rings (R).Head + 1);
            Dispatch (Item);
            Handled := Handled + 1;
         end loop;
      end loop;
   end Drain;

   function Others_Open return Boolean is
   begin
      for R in 1 .. Ring_Count - 1 loop
         if Open_Count (R) > 0 then
            return True;
         end if;
      end loop;
      return False;
   end Others_Open;

   procedure Check_Enter (RC : Integer) is
   begin
      if RC < 0 and then RC /= -Sys.EINTR and then RC /= -Sys.ETIME
        and then RC /= -Sys.EAGAIN and then RC /= -Sys.EBUSY
      then
         Fail ("io_uring_enter failed:" & RC'Image);
      end if;
   end Check_Enter;

   procedure Run_Uring (Listener : Integer; Available : out Boolean) is
      Limits : aliased Sys.Resource_Limit;
      RC : Integer;
      Handled : Natural := 0;
      Busy : Boolean;
      Last_Sweep : Unsigned_32;
      T0, T1, T2, T3, T4, T5, T6 : Unsigned_64 := 0;

      function Prepare (R : Natural) return Integer is
         Result : Integer := Uring.Setup (Rings (R), 4_096, 16_384);
      begin
         if Result = 0 then
            Result := Uring.Register_Sparse_Files (Rings (R), Slots);
         end if;
         if Result = 0 then
            Result := Uring.Setup_Buffers (Rings (R), Buffer_Count, Buffer_Size);
         end if;
         return Result;
      end Prepare;
   begin
      Available := False;
      if Sys.Get_Limit (7, Limits'Address) /= 0 then
         return;
      end if;
      -- A fixed-file table may not exceed the descriptor limit.
      Slots := Natural (unsigned_long'Min (Limits.Soft, Stride - 1));
      if Slots < 64 then
         return;
      end if;
      Margin := Natural'Min (512, Slots / 8);
      RC := Prepare (0);
      if RC /= 0 then
         Ada.Text_IO.Put_Line ("io_uring unavailable (errno" & Integer'Image (-RC) &
                               "); using epoll");
         return;
      end if;
      Ring_Count := 1;
      while Ring_Count < Max_Rings and then Prepare (Ring_Count) = 0 loop
         Ring_Count := Ring_Count + 1;
      end loop;
      Available := True;
      Mode := Use_Uring;
      Listen_FD := Listener;
      Ada.Text_IO.Put_Line ("io_uring:" & Ring_Count'Image & " rings of" & Slots'Image &
                            " direct sockets" &
                            (if Rings (0).Deferred then ", deferred task work" else ""));
      Now := Sys.Monotonic_Seconds;
      Last_Sweep := Now;
      Keep_Accepting;
      loop
         exit when Sys.Stop_Requested;
         T0 := Sys.Monotonic_Milliseconds;
         Busy := Handled >= Busy_Completions;
         Handled := 0;
         for R in 1 .. Ring_Count - 1 loop
            Check_Enter (Uring.Enter (Rings (R), Get_Events => False));
         end loop;
         T1 := Sys.Monotonic_Milliseconds;
         Check_Enter (Uring.Enter
           (Rings (0), Get_Events => True,
            Wait_For => (if Busy then Wait_Target else 1),
            Timeout => (if Busy then Batch_Nanos
                        elsif Others_Open or else Closing_Count > 0 or else Database.Writes_Held
                        then Idle_Nanos
                        else 1_000_000_000)));
         T2 := Sys.Monotonic_Milliseconds;
         for R in 1 .. Ring_Count - 1 loop
            Check_Enter (Uring.Enter (Rings (R), Get_Events => True));
         end loop;
         Now := Sys.Monotonic_Seconds;
         T3 := Sys.Monotonic_Milliseconds;
         Check_Gate;
         for R in 0 .. Ring_Count - 1 loop
            Drain (R, Handled);
         end loop;
         T4 := Sys.Monotonic_Milliseconds;
         End_Batch;
         T5 := Sys.Monotonic_Milliseconds;
         Stat_Worst_Batch := Unsigned_64'Max (Stat_Worst_Batch, T5 - T3);
         Stat_Batches := Stat_Batches + 1;
         Stat_Completions := Stat_Completions + Handled;
         if Now - Stat_Clock >= 10 then
            declare
               Total : Natural := 0;
            begin
               for R in 0 .. Ring_Count - 1 loop
                  Total := Total + Open_Count (R);
               end loop;
               Ada.Text_IO.Put_Line ("stats: open" & Total'Image & " accept_ring" & Accept_Ring'Image &
                 " batches" & Stat_Batches'Image & " cqes" & Stat_Completions'Image &
                 " worst_batch_ms" & Stat_Worst_Batch'Image & " worst_commit_ms" & Stat_Worst_Commit'Image &
                 " worst_sweep_ms" & Stat_Worst_Sweep'Image &
                 " enobufs" & Stat_No_Buffers'Image & " closing" & Closing_Count'Image &
                 Database.Checkpoint_Stats & " slow_loops" & Stat_Slow_Loops'Image &
                 (if Stat_Slow_Loops = 0 then "" else
                  " worst_loop_ms" & Stat_Worst_Loop'Image & " (submit" & Stat_Worst_Phases (1)'Image &
                  " wait" & Stat_Worst_Phases (2)'Image & " events" & Stat_Worst_Phases (3)'Image &
                  " drain" & Stat_Worst_Phases (4)'Image & " flush" & Stat_Worst_Phases (5)'Image &
                  " sql" & Stat_Worst_Phases (6)'Image & ")"));
               Ada.Text_IO.Flush;
            end;
            Stat_Clock := Now;
            Stat_Batches := 0; Stat_Completions := 0; Stat_No_Buffers := 0;
            Stat_Worst_Batch := 0; Stat_Worst_Commit := 0; Stat_Worst_Sweep := 0;
            Stat_Slow_Loops := 0; Stat_Worst_Loop := 0;
         end if;
         if Now /= Last_Sweep then
            declare
               Started : constant Unsigned_64 := Sys.Monotonic_Milliseconds;
            begin
               Sweep;
               Stat_Worst_Sweep := Unsigned_64'Max (Stat_Worst_Sweep, Sys.Monotonic_Milliseconds - Started);
            end;
            Last_Sweep := Now;
         end if;
         Keep_Accepting;
         T6 := Sys.Monotonic_Milliseconds;
         -- An idle wait is not a slow turn.
         if T6 - T0 - (if Handled = 0 then T2 - T1 else 0) >= Slow_Loop_Ms then
            Stat_Slow_Loops := Stat_Slow_Loops + 1;
            if T6 - T0 > Stat_Worst_Loop then
               Stat_Worst_Loop := T6 - T0;
               Stat_Worst_Phases := [T1 - T0, T2 - T1, T3 - T2, T4 - T3, T5 - T4, Last_SQL_Ms];
            end if;
         end if;
      end loop;
   end Run_Uring;

   ---------------------------------------------------------------------------
   -- epoll backend
   ---------------------------------------------------------------------------

   procedure Try_Send (ID : Conn_ID) is
      C : Conn renames Table (ID);
      N : long;
   begin
      if not C.Open or else C.Out_Block = System.Null_Address or else C.Deferred then
         return;
      end if;
      loop
         N := Sys.Send (int (ID), Sys.Offset (Pool.Data (C.Out_Block), Natural (C.Out_Offset)),
                        size_t (Pool.Length (C.Out_Block) - Natural (C.Out_Offset)), Sys.No_Signal);
         if N > 0 then
            C.Out_Offset := C.Out_Offset + Unsigned_32 (N);
            C.Last_Active := Now;
            if Natural (C.Out_Offset) >= Pool.Length (C.Out_Block) then
               Complete_Send (ID);
               return;
            end if;
         elsif N < 0 and then Sys.Errno = Sys.EINTR then
            null;
         elsif N < 0 and then Sys.Errno = Sys.EAGAIN then
            Set_Interest (ID, Writable);
            return;
         else
            Close_Conn (ID);
            return;
         end if;
      end loop;
   end Try_Send;

   procedure Service (ID : Conn_ID; Returned : Unsigned_32) is
      C : Conn renames Table (ID);
      N : long;
      Larger : System.Address;
   begin
      if (Returned and Readable) /= 0 and then not C.Writing then
         if C.Input = System.Null_Address then
            C.Input := Pool.Take (4_096);
            C.In_Length := 0;
         elsif Natural (C.In_Length) = Pool.Capacity (C.Input) and then
           C.In_Length < HTTP.Max_Input
         then
            Larger := Pool.Take (Natural'Min (HTTP.Max_Input, 2 * Natural (C.In_Length)));
            Sys.Copy (Pool.Data (Larger), Pool.Data (C.Input), size_t (C.In_Length));
            Pool.Give (C.Input);
            C.Input := Larger;
         end if;
         N := Sys.Receive (int (ID), Sys.Offset (Pool.Data (C.Input), Natural (C.In_Length)),
                           size_t (Natural'Min (Pool.Capacity (C.Input), HTTP.Max_Input) -
                             Natural (C.In_Length)), 0);
         if N = 0 then
            Close_Conn (ID);
            return;
         elsif N < 0 then
            if Sys.Errno /= Sys.EINTR and then Sys.Errno /= Sys.EAGAIN then
               Close_Conn (ID);
               return;
            end if;
         else
            if C.In_Length = 0 then
               C.Request_Start := Now;
            end if;
            C.In_Length := C.In_Length + Unsigned_32 (N);
            C.Last_Active := Now;
         end if;
         if C.In_Length > 0 then
            Process_Buffered (ID);
         else
            Pool.Give (C.Input);
            C.Input := System.Null_Address;
         end if;
      elsif (Returned and Writable) /= 0 then
         if C.Writing then
            if not C.Deferred then
               Queue_Work (ID);
            end if;
         elsif C.In_Length > 0 then
            Process_Buffered (ID);
            if C.Open and then not C.Writing then
               Set_Interest (ID, Readable);
            end if;
         else
            Set_Interest (ID, Readable);
         end if;
      end if;
      if (Returned and Broken) /= 0 then
         Close_Conn (ID);
      end if;
   end Service;

   procedure Run_Epoll (Listener : Integer) is
      Listener_Mark : constant Unsigned_32 := 16#FFFF_FFFF#;
      Events : aliased array (1 .. 512) of Ready_Event;
      Event : aliased Ready_Event;
      Count, Client : int;
      Ignored : int;
      One : aliased int := 1;
      Last_Sweep : Unsigned_32;
   begin
      Mode := Use_Epoll;
      Queue := Sys.Epoll_Create (524_288);
      if Queue < 0 then
         Fail ("epoll_create1 failed");
      end if;
      Event := (Readable, Listener_Mark, 0);
      if Sys.Epoll_Control (Queue, 1, int (Listener), Event'Address) /= 0 then
         Fail ("epoll_ctl listener failed");
      end if;
      Ada.Text_IO.Put_Line ("epoll: one event loop");
      Now := Sys.Monotonic_Seconds;
      Last_Sweep := Now;
      loop
         exit when Sys.Stop_Requested;
         Count := Sys.Epoll_Wait (Queue, Events'Address, Events'Length,
                                  (if Database.Writes_Held then 2 else 1_000));
         if Count < 0 and then Sys.Errno /= Sys.EINTR then
            Fail ("epoll_wait failed");
         end if;
         Now := Sys.Monotonic_Seconds;
         Check_Gate;
         for I in 1 .. Integer (Count) loop
            if Events (I).Slot = Listener_Mark then
               for Burst in 1 .. 64 loop
                  Client := Sys.Accept_Socket (int (Listener), System.Null_Address,
                                               System.Null_Address, Sys.Nonblock_Cloexec);
                  exit when Client < 0;
                  if Natural (Client) > Conn_ID'Last then
                     Ignored := Sys.Close (Client);
                  else
                     Open_Conn (Natural (Client));
                     Ignored := Sys.Set_Option (Client, 6, 1, One'Address, 4); -- TCP_NODELAY
                     Event := (Readable, Unsigned_32 (Client), Table (Natural (Client)).Gen);
                     if Sys.Epoll_Control (Queue, 1, Client, Event'Address) /= 0 then
                        Close_Conn (Natural (Client));
                     end if;
                     Highest_FD := Natural'Max (Highest_FD, Natural (Client));
                  end if;
               end loop;
            elsif Events (I).Slot <= Unsigned_32 (Conn_ID'Last) and then
              Table (Natural (Events (I).Slot)).Open and then
              Table (Natural (Events (I).Slot)).Gen = Events (I).Generation
            then
               Service (Natural (Events (I).Slot), Events (I).Events);
            end if;
         end loop;
         End_Batch;
         if Now /= Last_Sweep then
            Sweep;
            Last_Sweep := Now;
         end if;
      end loop;
   end Run_Epoll;
end Engine;
