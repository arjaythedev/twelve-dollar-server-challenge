with Ada.Environment_Variables;
with Ada.Exceptions;
with Ada.Text_IO;
with Interfaces.C; use Interfaces.C;
with System;
with Application;
with Database;
with Engine;
with Sys;

-- One process, one thread, one SQLite connection. Sockets, HTTP framing,
-- routing and JSON are handled in Ada; SQLite and OpenSSL are the only
-- application libraries.
procedure Server is
   type Padding is array (1 .. 8) of unsigned_char with Convention => C;
   type IPv4_Address is record
      Family : unsigned_short := 2;
      Port : unsigned_short;
      Address : unsigned := 0;
      Zero : Padding := [others => 0];
   end record with Convention => C;
   for IPv4_Address use record
      Family at 0 range 0 .. 15;
      Port at 2 range 0 .. 15;
      Address at 4 range 0 .. 31;
      Zero at 8 range 0 .. 63;
   end record;
   for IPv4_Address'Size use 128;

   Host : constant String := Ada.Environment_Variables.Value ("HOST", "127.0.0.1");
   Port : constant String := Ada.Environment_Variables.Value ("PORT", "3000");
   Bound : aliased IPv4_Address;
   One : aliased int := 1;
   Limits : aliased Sys.Resource_Limit;
   Listener : int;
   Available : Boolean;
   Ignored : System.Address;
begin
   -- Writes to a peer that has gone away must fail, not kill the process.
   Ignored := Sys.Set_Signal (13, Sys.Ignore_Signal);
   Ignored := Sys.Set_Signal (15, Sys.Request_Stop'Address);
   Ignored := Sys.Set_Signal (2, Sys.Request_Stop'Address);
   -- The descriptor limit also bounds each io_uring fixed-file table.
   if Sys.Get_Limit (7, Limits'Address) /= 0 then
      raise Program_Error with "getrlimit failed";
   end if;
   if Limits.Soft < unsigned_long'Min (Limits.Hard, 1_048_576) then
      Limits.Soft := unsigned_long'Min (Limits.Hard, 1_048_576);
      if Sys.Set_Limit (7, Limits'Address) /= 0 and then
        Sys.Get_Limit (7, Limits'Address) /= 0
      then
         raise Program_Error with "getrlimit failed";
      end if;
   end if;
   Ada.Text_IO.Put_Line ("Open-file soft limit:" & Limits.Soft'Image);

   Database.Open (Ada.Environment_Variables.Value ("SQLITE_PATH"));
   Application.Initialize (Ada.Environment_Variables.Value ("JWT_SECRET"));

   Bound.Port := Sys.Htons (unsigned_short'Value (Port));
   if Bound.Port = 0 then
      raise Constraint_Error with "PORT must be positive";
   end if;
   if Sys.Inet_Pton (2, To_C ((if Host = "localhost" then "127.0.0.1" else Host)),
                     Bound.Address'Address) /= 1
   then
      raise Constraint_Error with "HOST must be an IPv4 address or localhost";
   end if;
   Listener := Sys.Socket (2, 1 + Sys.Nonblock_Cloexec, 0);
   if Listener < 0 then
      raise Program_Error with "socket failed";
   end if;
   if Sys.Set_Option (Listener, 1, 2, One'Address, 4) /= 0 then -- SO_REUSEADDR
      raise Program_Error with "SO_REUSEADDR failed";
   end if;
   -- Accepted sockets inherit TCP_NODELAY from the listener; direct
   -- (io_uring) sockets have no descriptor to set it on afterwards.
   if Sys.Set_Option (Listener, 6, 1, One'Address, 4) /= 0 then
      raise Program_Error with "TCP_NODELAY failed";
   end if;
   -- A previous instance's sockets can outlive it briefly (io_uring tears
   -- rings down asynchronously), so wait for the port instead of failing.
   for Attempt in 1 .. 300 loop
      exit when Sys.Bind (Listener, Bound'Address, 16) = 0;
      if Sys.Errno /= Sys.EADDRINUSE or else Attempt = 300 then
         raise Program_Error with "bind failed";
      end if;
      delay 0.1;
   end loop;
   if Sys.Listen (Listener, 65_535) /= 0 then
      raise Program_Error with "listen failed";
   end if;
   Ada.Text_IO.Put_Line ("Ada server listening on " & Host & ":" & Port);

   Engine.Run_Uring (Integer (Listener), Available);
   if not Available then
      Engine.Run_Epoll (Integer (Listener));
   end if;
   -- Stopping: leave the LISTEN state at once so a successor can bind the
   -- port even while the kernel still holds this socket, then close SQLite
   -- (every acknowledged write is already committed).
   if Sys.Shutdown (Listener, 0) /= 0 then
      null;
   end if;
   Database.Close;
   Ada.Text_IO.Put_Line ("stopped");
exception
   when E : others =>
      Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error,
                            Ada.Exceptions.Exception_Information (E));
      Database.Close;
      raise;
end Server;
