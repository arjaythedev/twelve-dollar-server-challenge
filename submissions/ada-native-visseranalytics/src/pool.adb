with Interfaces.C;
with Sys;

package body Pool is
   use type System.Address;

   type Block_Header is record
      Next : System.Address;
      Class : Unsigned_32;
      Owner : Unsigned_32;
      Gen : Unsigned_32;
      Length : Unsigned_32;
      Reserved : Unsigned_64;
   end record with Convention => C;
   for Block_Header'Size use Header_Bytes * 8;

   Smallest_Shift : constant := 8;   -- 256 bytes
   type Class_Index is range 0 .. 13; -- up to 2 MiB
   Free_List : array (Class_Index) of System.Address := [others => System.Null_Address];
   Free_Count : array (Class_Index) of Natural := [others => 0];
   -- Enough spare blocks for steady traffic (a few hundred responses are in
   -- flight at once); the surplus left by a burst goes back to malloc, and
   -- Trim returns it to the kernel.
   Keep : constant array (Class_Index) of Natural :=
     [0 .. 5 => 1_024, 6 .. 8 => 64, others => 8];
   Released : Natural := 0;   -- blocks given back to malloc since the last trim

   function Malloc_Trim (Pad : Interfaces.C.size_t) return Interfaces.C.int
     with Import, Convention => C, External_Name => "malloc_trim";

   procedure Trim is
      Ignored : Interfaces.C.int;
   begin
      if Released >= 256 then
         Ignored := Malloc_Trim (0);
         Released := 0;
      end if;
   end Trim;

   function Class_Bytes (C : Class_Index) return Natural is (2 ** (Natural (C) + Smallest_Shift));

   function Take (Bytes : Natural) return System.Address is
      C : Class_Index := 0;
      Block : System.Address;
   begin
      if Bytes > Largest then
         raise Constraint_Error with "block too large";
      end if;
      while Class_Bytes (C) < Bytes loop
         C := C + 1;
      end loop;
      Block := Free_List (C);
      if Block /= System.Null_Address then
         declare
            H : Block_Header with Import, Address => Block;
         begin
            Free_List (C) := H.Next;
            Free_Count (C) := Free_Count (C) - 1;
            H.Length := 0;
         end;
         return Block;
      end if;
      Block := Sys.Malloc (Interfaces.C.size_t (Header_Bytes + Class_Bytes (C)));
      if Block = System.Null_Address then
         raise Storage_Error with "out of memory";
      end if;
      declare
         H : Block_Header with Import, Address => Block;
      begin
         H := (Next => System.Null_Address, Class => Unsigned_32 (C),
               Owner => 0, Gen => 0, Length => 0, Reserved => 0);
      end;
      return Block;
   end Take;

   procedure Give (Block : System.Address) is
      H : Block_Header with Import, Address => Block;
      C : constant Class_Index := Class_Index (H.Class);
   begin
      if Free_Count (C) < Keep (C) then
         H.Next := Free_List (C);
         Free_List (C) := Block;
         Free_Count (C) := Free_Count (C) + 1;
      else
         Sys.Free (Block);
         Released := Released + 1;
      end if;
   end Give;

   function Data (Block : System.Address) return System.Address is
     (Sys.Offset (Block, Header_Bytes));

   function Capacity (Block : System.Address) return Natural is
      H : Block_Header with Import, Address => Block;
   begin
      return Class_Bytes (Class_Index (H.Class));
   end Capacity;

   procedure Set_Owner (Block : System.Address; Owner, Gen : Unsigned_32) is
      H : Block_Header with Import, Address => Block;
   begin
      H.Owner := Owner;
      H.Gen := Gen;
   end Set_Owner;

   function Owner (Block : System.Address) return Unsigned_32 is
      H : Block_Header with Import, Address => Block;
   begin
      return H.Owner;
   end Owner;

   function Gen (Block : System.Address) return Unsigned_32 is
      H : Block_Header with Import, Address => Block;
   begin
      return H.Gen;
   end Gen;

   procedure Set_Length (Block : System.Address; Length : Natural) is
      H : Block_Header with Import, Address => Block;
   begin
      H.Length := Unsigned_32 (Length);
   end Set_Length;

   function Length (Block : System.Address) return Natural is
      H : Block_Header with Import, Address => Block;
   begin
      return Natural (H.Length);
   end Length;
end Pool;
