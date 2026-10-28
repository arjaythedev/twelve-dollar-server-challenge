with Interfaces.C; use Interfaces.C;
with System;
with System.Storage_Elements;

package body Workers is
   use type System.Address;
   Gate : System.Address := System.Null_Address;
   function Map_Shared (Address : System.Address; Length : size_t;
                        Protection, Flags, FD : int; Offset : long) return System.Address
     with Import, Convention => C, External_Name => "mmap";
   function Attr_Init (Attributes : System.Address) return int
     with Import, Convention => C, External_Name => "pthread_mutexattr_init";
   function Attr_Shared (Attributes : System.Address; Value : int) return int
     with Import, Convention => C, External_Name => "pthread_mutexattr_setpshared";
   function Attr_Protocol (Attributes : System.Address; Value : int) return int
     with Import, Convention => C, External_Name => "pthread_mutexattr_setprotocol";
   function Attr_Destroy (Attributes : System.Address) return int
     with Import, Convention => C, External_Name => "pthread_mutexattr_destroy";
   function Mutex_Init (Mutex, Attributes : System.Address) return int
     with Import, Convention => C, External_Name => "pthread_mutex_init";
   function Mutex_Lock (Mutex : System.Address) return int
     with Import, Convention => C, External_Name => "pthread_mutex_lock";
   function Mutex_Unlock (Mutex : System.Address) return int
     with Import, Convention => C, External_Name => "pthread_mutex_unlock";
   function Fork return int
     with Import, Convention => C, External_Name => "fork";
   function Get_PID return int
     with Import, Convention => C, External_Name => "getpid";
   function Get_Parent_PID return int
     with Import, Convention => C, External_Name => "getppid";
   function Prctl (Option : int; Arg2, Arg3, Arg4, Arg5 : unsigned_long) return int
     with Import, Convention => C, External_Name => "prctl";
   function Kill (PID, Signal : int) return int
     with Import, Convention => C, External_Name => "kill";
   function Wait_PID (PID : int; Status : System.Address; Options : int) return int
     with Import, Convention => C, External_Name => "waitpid";
   function Errno return access int
     with Import, Convention => C, External_Name => "__errno_location";
   procedure Exit_Immediately (Status : int)
     with Import, Convention => C, External_Name => "_exit", No_Return;

   procedure Acquire is
   begin
      if Gate /= System.Null_Address and then Mutex_Lock (Gate) /= 0 then
         raise Program_Error with "worker gate lock failed";
      end if;
   end Acquire;

   procedure Release is
   begin
      if Gate /= System.Null_Address and then Mutex_Unlock (Gate) /= 0 then
         raise Program_Error with "worker gate release failed";
      end if;
   end Release;

   procedure Start (Count : Positive) is
      Children : array (1 .. Count) of int := (others => 0);
      Parent : constant int := Get_PID;
      Child, Ended, Ignored : int;
      Status : aliased int;
      -- Opaque, naturally aligned storage larger than glibc's attributes on
      -- both supported 64-bit architectures. Only pthread functions access it.
      Attributes : aliased array (1 .. 8) of unsigned_long := (others => 0);
      use type System.Storage_Elements.Integer_Address;
   begin
      if Count = 1 then return; end if;
      Gate := Map_Shared (System.Null_Address, 4096, 3, 1 + 32, -1, 0);
      if System.Storage_Elements.To_Integer (Gate) = System.Storage_Elements.Integer_Address'Last then
         raise Program_Error with "worker gate mapping failed";
      end if;
      -- A process-shared PI mutex hands the lock to a waiting worker rather
      -- than letting the current worker repeatedly steal a semaphore token.
      -- Priorities and CPU affinity remain unchanged. The anonymous mapping
      -- disappears when the supervisor and its workers exit.
      if Attr_Init (Attributes'Address) /= 0 or else
        Attr_Shared (Attributes'Address, 1) /= 0 or else
        Attr_Protocol (Attributes'Address, 1) /= 0 or else
        Mutex_Init (Gate, Attributes'Address) /= 0 or else
        Attr_Destroy (Attributes'Address) /= 0
      then
         raise Program_Error with "worker gate initialization failed";
      end if;
      for I in Children'Range loop
         Child := Fork;
         if Child = 0 then
            -- A killed supervisor must not leave orphan servers listening.
            -- Check parent identity after prctl to cover death during fork.
            if Prctl (1, 15, 0, 0, 0) /= 0 or else Get_Parent_PID /= Parent then
               Exit_Immediately (1);
            end if;
            return;
         elsif Child < 0 then
            for PID of Children loop
               if PID > 0 then Ignored := Kill (PID, 15); end if;
            end loop;
            raise Program_Error with "cannot start server workers";
         end if;
         Children (I) := Child;
      end loop;
      loop
         Ended := Wait_PID (-1, Status'Address, 0);
         exit when Ended >= 0 or else Errno.all /= 4;
      end loop;
      for PID of Children loop
         if PID /= Ended then Ignored := Kill (PID, 15); end if;
      end loop;
      for PID of Children loop
         if PID /= Ended then
            loop
               Ignored := Wait_PID (PID, Status'Address, 0);
               exit when Ignored >= 0 or else Errno.all /= 4;
            end loop;
         end if;
      end loop;
      raise Program_Error with "server worker exited";
   end Start;
end Workers;
