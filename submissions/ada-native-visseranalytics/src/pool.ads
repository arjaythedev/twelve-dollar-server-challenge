with Interfaces; use Interfaces;
with System;

-- Power-of-two blocks for responses and partial requests. Each block starts
-- with a small header; its data follows. Freed blocks go on per-size LIFO
-- lists, so steady-state requests allocate nothing.
package Pool is
   Header_Bytes : constant := 32;
   Largest : constant := 2 ** 21;

   -- Returns a block whose data holds at least Bytes. Never null.
   function Take (Bytes : Natural) return System.Address;
   procedure Give (Block : System.Address);
   -- Returns freed heap memory to the kernel after a burst (call rarely).
   procedure Trim;

   function Data (Block : System.Address) return System.Address;
   function Capacity (Block : System.Address) return Natural;

   -- Owner fields let a completion find its connection after the
   -- connection slot may have been reused.
   procedure Set_Owner (Block : System.Address; Owner, Gen : Unsigned_32);
   function Owner (Block : System.Address) return Unsigned_32;
   function Gen (Block : System.Address) return Unsigned_32;
   procedure Set_Length (Block : System.Address; Length : Natural);
   function Length (Block : System.Address) return Natural;
end Pool;
