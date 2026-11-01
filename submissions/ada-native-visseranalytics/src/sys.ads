with Interfaces; use Interfaces;
with Interfaces.C; use Interfaces.C;
with System;
with System.Storage_Elements;

-- Linux and libc bindings. Only the ABI lives here; framing, routing and
-- event handling are written in Ada.
package Sys is

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
   function Shutdown (FD, How : int) return int
     with Import, Convention => C, External_Name => "shutdown";
   function Inet_Pton (Family : int; Text : char_array;
                       Address : System.Address) return int
     with Import, Convention => C, External_Name => "inet_pton";
   function Htons (Port : unsigned_short) return unsigned_short
     with Import, Convention => C, External_Name => "htons";
   function Errno_Location return access int
     with Import, Convention => C, External_Name => "__errno_location";
   function Errno return int is (Errno_Location.all);

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

   function Map (Address : System.Address; Length : size_t;
                 Protection, Flags, FD : int; Offset : long) return System.Address
     with Import, Convention => C, External_Name => "mmap";
   function Unmap (Address : System.Address; Length : size_t) return int
     with Import, Convention => C, External_Name => "munmap";
   Map_Failed : constant System.Address :=
     System.Storage_Elements.To_Address (System.Storage_Elements.Integer_Address'Last);

   function Malloc (Size : size_t) return System.Address
     with Import, Convention => C, External_Name => "malloc";
   procedure Free (Address : System.Address)
     with Import, Convention => C, External_Name => "free";
   procedure Copy (Target, Source : System.Address; Count : size_t)
     with Import, Convention => C, External_Name => "memcpy";
   procedure Move (Target, Source : System.Address; Count : size_t)
     with Import, Convention => C, External_Name => "memmove";

   -- Set by SIGTERM or SIGINT; the event loop stops at its next turn.
   Stop_Requested : Boolean := False with Volatile;
   procedure Request_Stop (Signal : int) with Convention => C;
   function Set_Signal (Signal : int; Handler : System.Address) return System.Address
     with Import, Convention => C, External_Name => "signal";
   Ignore_Signal : constant System.Address := System.Storage_Elements.To_Address (1);

   -- syscall(2) is variadic in C; C_Variadic_1 keeps the ABI exact.
   function Syscall (Number : long; A1, A2, A3, A4, A5, A6 : long := 0) return long
     with Import, Convention => C_Variadic_1, External_Name => "syscall";

   function To_Long (Address : System.Address) return long is
     (long (System.Storage_Elements.To_Integer (Address)));
   function Offset (Address : System.Address; Bytes : Natural) return System.Address is
     (System.Storage_Elements."+" (Address, System.Storage_Elements.Storage_Offset (Bytes)));

   -- Seconds since an arbitrary start, from the monotonic coarse clock.
   function Monotonic_Seconds return Unsigned_32;
   -- Monotonic milliseconds (wraps), for rate limits.
   function Monotonic_Milliseconds return Unsigned_64;
   -- Wall clock seconds, as a float, for JWT expiry checks.
   function Unix_Time return Long_Long_Float;

   EINTR : constant := 4;
   EADDRINUSE : constant := 98;
   EAGAIN : constant := 11;
   ENOBUFS : constant := 105;
   ECANCELED : constant := 125;
   ETIME : constant := 62;
   EBUSY : constant := 16;
   ENFILE : constant := 23;
   EMFILE : constant := 24;
   No_Signal : constant := 16#4000#;
   Nonblock_Cloexec : constant := 2048 + 524288;
end Sys;
