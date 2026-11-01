package body Sys is
   type Timespec is record
      Seconds, Nanoseconds : long;
   end record with Convention => C;
   function Get_Time (Clock_ID : int; Stamp : access Timespec) return int
     with Import, Convention => C, External_Name => "clock_gettime";

   Realtime : constant := 0;
   Monotonic_Coarse : constant := 6;

   function Monotonic_Seconds return Unsigned_32 is
      Stamp : aliased Timespec;
   begin
      if Get_Time (Monotonic_Coarse, Stamp'Access) /= 0 then
         raise Program_Error with "clock_gettime failed";
      end if;
      return Unsigned_32 (Stamp.Seconds mod 2**32);
   end Monotonic_Seconds;

   function Monotonic_Milliseconds return Unsigned_64 is
      Stamp : aliased Timespec;
   begin
      if Get_Time (1, Stamp'Access) /= 0 then
         raise Program_Error with "clock_gettime failed";
      end if;
      return Unsigned_64 (Stamp.Seconds) * 1000 + Unsigned_64 (Stamp.Nanoseconds / 1_000_000);
   end Monotonic_Milliseconds;

   procedure Request_Stop (Signal : int) is
      pragma Unreferenced (Signal);
   begin
      Stop_Requested := True;
   end Request_Stop;

   function Unix_Time return Long_Long_Float is
      Stamp : aliased Timespec;
   begin
      if Get_Time (Realtime, Stamp'Access) /= 0 then
         raise Program_Error with "clock_gettime failed";
      end if;
      return Long_Long_Float (Stamp.Seconds) +
        Long_Long_Float (Stamp.Nanoseconds) / 1_000_000_000.0;
   end Unix_Time;
end Sys;
