with Ada.Environment_Variables;
with Ada.Exceptions;
with Ada.Real_Time; use Ada.Real_Time;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Ada.Unchecked_Deallocation;
with Interfaces.C; use Interfaces.C;
with System;
with Application;
with Database;
with HTTP;

procedure Server is
   -- Linux libc ABI bindings only. All framing, routing and application logic
   -- live in Ada; SQLite and OpenSSL are the only application libraries.
   function Socket (Domain, Kind, Protocol : int) return int
     with Import, Convention => C, External_Name => "socket";
   function Bind (FD : int; Address : System.Address; Length : unsigned) return int
     with Import, Convention => C, External_Name => "bind";
   function Listen (FD, Backlog : int) return int
     with Import, Convention => C, External_Name => "listen";
   function Accept_Socket (FD : int; Address, Length : System.Address;
                           Flags : int) return int
     with Import, Convention => C, External_Name => "accept4";
   function Set_Option (FD, Level, Option : int; Value : System.Address;
                        Length : unsigned) return int
     with Import, Convention => C, External_Name => "setsockopt";
   function Close (FD : int) return int
     with Import, Convention => C, External_Name => "close";
   function Receive (FD : int; Buffer : System.Address; Length : size_t;
                     Flags : int) return long
     with Import, Convention => C, External_Name => "recv";
   function Send (FD : int; Buffer : System.Address; Length : size_t;
                  Flags : int) return long
     with Import, Convention => C, External_Name => "send";
   function Inet_Pton (Family : int; Text : char_array;
                       Address : System.Address) return int
     with Import, Convention => C, External_Name => "inet_pton";
   function Htons (Port : unsigned_short) return unsigned_short
     with Import, Convention => C, External_Name => "htons";
   function Errno return access int
     with Import, Convention => C, External_Name => "__errno_location";
   function Epoll_Create (Flags : int) return int
     with Import, Convention => C, External_Name => "epoll_create1";
   function Epoll_Control (Queue, Operation, FD : int; Event : System.Address) return int
     with Import, Convention => C, External_Name => "epoll_ctl";
   function Epoll_Wait (Queue : int; Events : System.Address;
                        Count, Timeout : int) return int
     with Import, Convention => C, External_Name => "epoll_wait";
   type Resource_Limit is record
      Soft, Hard : unsigned_long;
   end record with Convention => C;
   function Get_Limit (Resource : int; Limits : System.Address) return int
     with Import, Convention => C, External_Name => "getrlimit";
   function Set_Limit (Resource : int; Limits : System.Address) return int
     with Import, Convention => C, External_Name => "setrlimit";

   type Padding is array (1 .. 8) of unsigned_char with Convention => C;
   type IPv4_Address is record
      Family : unsigned_short := 2;
      Port : unsigned_short;
      Address : unsigned := 0;
      Zero : Padding := (others => 0);
   end record with Convention => C;
   for IPv4_Address use record
      Family at 0 range 0 .. 15;
      Port at 2 range 0 .. 15;
      Address at 4 range 0 .. 31;
      Zero at 8 range 0 .. 63;
   end record;
   for IPv4_Address'Size use 128;

   -- Linux x86_64 epoll_event is a packed 12-byte record. The user data
   -- carries a slot AND generation so a stale event cannot hit a reused slot.
   type Ready_Event is record
      Events : unsigned := 0;
      Slot : unsigned := 0;
      Generation : unsigned := 0;
   end record with Convention => C;
   for Ready_Event use record
      Events at 0 range 0 .. 31;
      Slot at 4 range 0 .. 31;
      Generation at 8 range 0 .. 31;
   end record;
   for Ready_Event'Size use 96;
   for Ready_Event'Alignment use 4;

   Max_Connections : constant := 65_000;
   Readable : constant unsigned := 1;
   Writable : constant unsigned := 4;
   Broken : constant unsigned := 8 + 16;
   Nonblock_Cloexec : constant int := 2048 + 524288;
   No_Signal : constant int := 16#4000#;
   Idle_Lifetime : constant Time_Span := Seconds (75);
   Request_Lifetime : constant Time_Span := Seconds (30);
   type String_Access is access String;
   procedure Free_String is new Ada.Unchecked_Deallocation (String, String_Access);
   type Connection is record
      FD : int := -1;
      Interest : unsigned := Readable;
      -- Idle connections carry no input buffer. Grow partial requests only
      -- as needed instead of reserving 64 KiB for every connected user.
      Input : String_Access := null;
      Used : Natural := 0;
      Output : String_Access := null;
      Sent : Natural := 0;
      Closing : Boolean := False;
      Interim : Boolean := False;
      Continue_Sent : Boolean := False;
      Buffered_Work : Boolean := False;
      Last_Active : Time := Clock;
      Request_Start : Time := Clock;
   end record;
   type Connection_Access is access Connection;
   procedure Free_Connection is new Ada.Unchecked_Deallocation (Connection, Connection_Access);
   Connections : array (1 .. Max_Connections) of Connection_Access := (others => null);
   Generations : array (1 .. Max_Connections) of unsigned := (others => 0);
   Free_Slots : array (1 .. Max_Connections) of Positive;
   Free_Count : Natural := Max_Connections;
   Events : aliased array (1 .. 256) of Ready_Event;
   Event : aliased Ready_Event;
   Highest : Natural := 0;
   Listener : int := -1;
   Queue : int := -1;
   One : aliased int := 1;
   Bound : aliased IPv4_Address;
   File_Limit : aliased Resource_Limit;
   RC, Client, Ready_Count : int;
   N : long;
   Now, Last_Sweep : Time := Clock;
   Host : constant String := Ada.Environment_Variables.Value ("HOST", "127.0.0.1");
   Port : constant String := Ada.Environment_Variables.Value ("PORT", "3000");

   procedure Drop (I : Positive) is
      Ignored : int;
   begin
      if Connections (I) /= null then
         Ignored := Epoll_Control (Queue, 2, Connections (I).FD, System.Null_Address);
         Ignored := Close (Connections (I).FD);
         Free_String (Connections (I).Input);
         Free_String (Connections (I).Output);
         Free_Connection (Connections (I));
         Free_Count := Free_Count + 1;
         Free_Slots (Free_Count) := I;
      end if;
   end Drop;

   procedure Queue_Error (C : Connection_Access; Code : Positive; Message : String) is
   begin
      C.Closing := True;
      C.Output := new String'(HTTP.Serialize (Application.Error (Code, Message), True));
      C.Sent := 0;
   end Queue_Error;

   procedure Try_Request (C : Connection_Access) is
      Item : HTTP.Request;
      Status : HTTP.Parse_Status;
      Consumed : Natural;
   begin
      C.Buffered_Work := False;
      HTTP.Parse (C.Input (1 .. C.Used), Item, Status, Consumed);
      case Status is
         when HTTP.Incomplete =>
            if C.Used = HTTP.Max_Input then
               Queue_Error (C, 413, "request too large");
            elsif Item.Expect_Continue and then not C.Continue_Sent then
               C.Continue_Sent := True; C.Interim := True;
               C.Output := new String'("HTTP/1.1 100 Continue" &
                 ASCII.CR & ASCII.LF & ASCII.CR & ASCII.LF);
               C.Sent := 0;
            end if;
         when HTTP.Bad_Request => Queue_Error (C, 400, "bad request");
         when HTTP.Too_Large => Queue_Error (C, 413, "request too large");
         when HTTP.Complete =>
            C.Closing := Item.Close;
            C.Output := new String'(HTTP.Serialize
              (Application.Handle (To_String (Item.Method), To_String (Item.Path),
               To_String (Item.Authorization), To_String (Item.Payload)), C.Closing));
            C.Sent := 0; C.Continue_Sent := False;
            C.Input (1 .. C.Used - Consumed) := C.Input (Consumed + 1 .. C.Used);
            C.Used := C.Used - Consumed;
            C.Request_Start := Clock;
      end case;
   end Try_Request;

   procedure Service (I : Positive; Returned : unsigned) is
      C : constant Connection_Access := Connections (I);
      Interest : unsigned;
      Change : aliased Ready_Event;
   begin
      if (Returned and Readable) /= 0 and then C.Output = null then
         if C.Used = 0 then C.Request_Start := Clock; end if;
         if C.Input = null then
            C.Input := new String (1 .. 4096);
         elsif C.Used = C.Input'Length and then C.Used < HTTP.Max_Input then
            declare
               Larger : constant String_Access := new String
                 (1 .. Natural'Min (HTTP.Max_Input, C.Input'Length * 2));
            begin
               Larger (1 .. C.Used) := C.Input (1 .. C.Used);
               Free_String (C.Input);
               C.Input := Larger;
            end;
         end if;
         N := Receive (C.FD, C.Input (C.Used + 1)'Address,
                       size_t (C.Input'Length - C.Used), 0);
         if N = 0 then Drop (I); return;
         elsif N < 0 then
            if Errno.all not in 4 | 11 then Drop (I); return; end if;
         else
            C.Used := C.Used + Natural (N); C.Last_Active := Clock;
         end if;
      end if;
      -- A bounded pipeline burst prevents one busy client starving its peers.
      for Burst in 1 .. 4 loop
         if C.Output = null and then C.Used > 0 then Try_Request (C); end if;
         exit when C.Output = null;
         N := Send (C.FD, C.Output (C.Output'First + C.Sent)'Address,
                    size_t (C.Output'Length - C.Sent), No_Signal);
         if N < 0 then
            if Errno.all not in 4 | 11 then Drop (I); return; end if;
            exit;
         elsif N = 0 then exit;
         end if;
         C.Sent := C.Sent + Natural (N); C.Last_Active := Clock;
         exit when C.Sent < C.Output'Length;
         Free_String (C.Output); C.Sent := 0;
         if C.Closing then Drop (I); return; end if;
         if C.Interim then
            C.Interim := False;
            exit;
         end if;
         C.Buffered_Work := C.Used > 0;
         exit when not C.Buffered_Work;
      end loop;
      if (Returned and Broken) /= 0 then Drop (I); return; end if;
      if C.Used = 0 then
         Free_String (C.Input);
      end if;
      -- Level-triggered writable readiness also schedules a bounded pipeline
      -- continuation when the next request is already in our user buffer.
      Interest := (if C.Output /= null or else C.Buffered_Work then Writable else Readable);
      if Interest /= C.Interest then
         Change := (Interest, unsigned (I), Generations (I));
         RC := Epoll_Control (Queue, 3, C.FD, Change'Address);
         if RC /= 0 then Drop (I); return; end if;
         C.Interest := Interest;
      end if;
   exception
      when E : others =>
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error,
           "connection failed: " & Ada.Exceptions.Exception_Information (E));
         Drop (I);
   end Service;
begin
   RC := Get_Limit (7, File_Limit'Address); -- RLIMIT_NOFILE on Linux
   if RC /= 0 then raise Program_Error with "getrlimit failed"; end if;
   if File_Limit.Soft < 65_535 then
      File_Limit.Soft := unsigned_long'Min (65_535, File_Limit.Hard);
      RC := Set_Limit (7, File_Limit'Address);
      if RC /= 0 then raise Program_Error with "setrlimit failed"; end if;
   end if;
   Ada.Text_IO.Put_Line ("Open-file soft limit:" & File_Limit.Soft'Image);
   for I in Free_Slots'Range loop
      Free_Slots (I) := Max_Connections - I + 1;
   end loop;
   Database.Open (Ada.Environment_Variables.Value ("SQLITE_PATH"));
   Application.Initialize (Ada.Environment_Variables.Value ("JWT_SECRET"));
   Bound.Port := Htons (unsigned_short'Value (Port));
   if Bound.Port = 0 then raise Constraint_Error with "PORT must be positive"; end if;
   RC := Inet_Pton (2, To_C ((if Host = "localhost" then "127.0.0.1" else Host)),
                    Bound.Address'Address);
   if RC /= 1 then raise Constraint_Error with "HOST must be an IPv4 address or localhost"; end if;
   Listener := Socket (2, 1 + Nonblock_Cloexec, 0);
   if Listener < 0 then raise Program_Error with "socket failed"; end if;
   RC := Set_Option (Listener, 1, 2, One'Address, 4);
   if RC /= 0 then raise Program_Error with "SO_REUSEADDR failed"; end if;
   RC := Bind (Listener, Bound'Address, 16);
   if RC /= 0 then raise Program_Error with "bind failed"; end if;
   RC := Listen (Listener, 1024);
   if RC /= 0 then raise Program_Error with "listen failed"; end if;
   Queue := Epoll_Create (524288);
   if Queue < 0 then raise Program_Error with "epoll_create1 failed"; end if;
   Event := (Readable, 0, 0);
   RC := Epoll_Control (Queue, 1, Listener, Event'Address);
   if RC /= 0 then raise Program_Error with "epoll_ctl listener failed"; end if;
   Ada.Text_IO.Put_Line ("Ada server listening on " & Host & ":" & Port);
   loop
      Ready_Count := Epoll_Wait (Queue, Events'Address, Events'Length, 1000);
      if Ready_Count < 0 and then Errno.all /= 4 then
         raise Program_Error with "epoll_wait failed";
      end if;
      for Ready in 1 .. Ready_Count loop
        if Events (Integer (Ready)).Slot = 0 then
         for Burst in 1 .. 64 loop
            Client := Accept_Socket (Listener, System.Null_Address,
                                     System.Null_Address, Nonblock_Cloexec);
            exit when Client < 0;
            declare
               Slot : Positive;
            begin
               if Free_Count = 0 then RC := Close (Client);
               else
                  Slot := Free_Slots (Free_Count);
                  Free_Count := Free_Count - 1;
                  Generations (Slot) := Generations (Slot) + 1;
                  Connections (Slot) := new Connection;
                  Connections (Slot).FD := Client;
                  RC := Set_Option (Client, 6, 1, One'Address, 4); -- TCP_NODELAY
                  Event := (Readable, unsigned (Slot), Generations (Slot));
                  RC := Epoll_Control (Queue, 1, Client, Event'Address);
                  if RC /= 0 then Drop (Slot); end if;
                  Highest := Natural'Max (Highest, Slot);
               end if;
            end;
         end loop;
        else
           declare
              I : constant Positive := Positive (Events (Integer (Ready)).Slot);
           begin
              if Connections (I) /= null and then
                Events (Integer (Ready)).Generation = Generations (I)
              then
                 Service (I, Events (Integer (Ready)).Events);
              end if;
           end;
        end if;
      end loop;
      Now := Clock;
      if Now - Last_Sweep >= Seconds (1) then
         for I in 1 .. Highest loop
            if Connections (I) /= null then
               declare
                  C : constant Connection_Access := Connections (I);
               begin
                  if Now - C.Last_Active >= Idle_Lifetime or else
                    ((C.Used > 0 or else C.Output /= null) and then
                     Now - C.Request_Start >= Request_Lifetime)
                  then
                     Drop (I);
                  end if;
               end;
            end if;
         end loop;
         Last_Sweep := Now;
      end if;
      while Highest > 0 and then Connections (Highest) = null loop
         Highest := Highest - 1;
      end loop;
   end loop;
exception
   when E : others =>
      Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, Ada.Exceptions.Exception_Information (E));
      for I in 1 .. Highest loop
         if Connections (I) /= null then Drop (I); end if;
      end loop;
      if Listener >= 0 then RC := Close (Listener); end if;
      if Queue >= 0 then RC := Close (Queue); end if;
      Database.Close;
      raise;
end Server;
