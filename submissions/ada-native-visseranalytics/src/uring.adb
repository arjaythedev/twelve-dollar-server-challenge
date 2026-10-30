with Interfaces.C; use Interfaces.C;
with System.Storage_Elements;
with Sys;

package body Uring is
   use type System.Address;

   -- GCC atomics with explicit ordering for the shared ring indices.
   function Load_32 (Ptr : System.Address; Model : Integer) return Unsigned_32
     with Import, Convention => Intrinsic, External_Name => "__atomic_load_4";
   procedure Store_32 (Ptr : System.Address; Value : Unsigned_32; Model : Integer)
     with Import, Convention => Intrinsic, External_Name => "__atomic_store_4";
   procedure Store_16 (Ptr : System.Address; Value : Unsigned_16; Model : Integer)
     with Import, Convention => Intrinsic, External_Name => "__atomic_store_2";
   Acquire : constant := 2;
   Release : constant := 3;

   function To_U64 (Address : System.Address) return Unsigned_64 is
     (Unsigned_64 (System.Storage_Elements.To_Integer (Address)));

   -- struct io_uring_sqe (64 bytes).
   type Submission is record
      Opcode : Unsigned_8;
      Flags : Unsigned_8;
      Priority : Unsigned_16;
      FD : Integer_32;
      Offset : Unsigned_64;
      Address : Unsigned_64;
      Length : Unsigned_32;
      Op_Flags : Unsigned_32;
      User_Data : Unsigned_64;
      Buffer_Group : Unsigned_16;
      Personality : Unsigned_16;
      File_Index : Unsigned_32;
      Address_3 : Unsigned_64;
      Padding : Unsigned_64;
   end record with Convention => C;
   for Submission use record
      Opcode at 0 range 0 .. 7;
      Flags at 1 range 0 .. 7;
      Priority at 2 range 0 .. 15;
      FD at 4 range 0 .. 31;
      Offset at 8 range 0 .. 63;
      Address at 16 range 0 .. 63;
      Length at 24 range 0 .. 31;
      Op_Flags at 28 range 0 .. 31;
      User_Data at 32 range 0 .. 63;
      Buffer_Group at 40 range 0 .. 15;
      Personality at 42 range 0 .. 15;
      File_Index at 44 range 0 .. 31;
      Address_3 at 48 range 0 .. 63;
      Padding at 56 range 0 .. 63;
   end record;
   for Submission'Size use 512;
   Zero : constant Submission :=
     (Opcode | Flags => 0, Priority | Buffer_Group | Personality => 0, FD => 0,
      Length | Op_Flags | File_Index => 0,
      Offset | Address | User_Data | Address_3 | Padding => 0);

   Op_Accept : constant := 13;
   Op_Cancel : constant := 14;
   Op_Close : constant := 19;
   Op_Send : constant := 26;
   Op_Receive : constant := 27;
   Fixed_File : constant := 1;
   Buffer_Select : constant := 32;

   type Words is array (0 .. 29) of Unsigned_32 with Convention => C;

   -- CQSIZE | SUBMIT_ALL, plus SINGLE_ISSUER | DEFER_TASKRUN (Linux 6.1+):
   -- completions are processed only when this thread asks for them, which
   -- batches network work. Older kernels fall back to COOP_TASKRUN.
   Deferred_Flags : constant := 8 + 128 + 4096 + 8192;
   Basic_Flags : constant := 8 + 128 + 256;

   function At_Offset (Base : System.Address; Bytes : Unsigned_32) return System.Address is
     (Sys.Offset (Base, Natural (Bytes)));

   function Setup (R : in out Ring; Entries, Completions : Positive) return Integer is
      Params : aliased Words := [others => 0];
      FD : long;
      Base, Entries_Base : System.Address;
   begin
      Params (1) := Unsigned_32 (Completions);
      Params (2) := Deferred_Flags;
      FD := Sys.Syscall (425, long (Entries), Sys.To_Long (Params'Address));
      R.Deferred := FD >= 0;
      if FD < 0 then
         Params := [others => 0];
         Params (1) := Unsigned_32 (Completions);
         Params (2) := Basic_Flags;
         FD := Sys.Syscall (425, long (Entries), Sys.To_Long (Params'Address));
         if FD < 0 then
            return -Integer (Sys.Errno);
         end if;
      end if;
      R.FD := Integer (FD);
      -- IORING_FEAT_SINGLE_MMAP and IORING_FEAT_EXT_ARG.
      if (Params (5) and 1) = 0 or else (Params (5) and 256) = 0 then
         return -22;
      end if;
      declare
         SQ_Count : constant Natural := Natural (Params (0));
         CQ_Count : constant Natural := Natural (Params (1));
         Ring_Bytes : constant Natural :=
           Natural'Max (Natural (Params (16)) + SQ_Count * 4, Natural (Params (25)) + CQ_Count * 16);
      begin
         -- MAP_SHARED | MAP_POPULATE
         Base := Sys.Map (System.Null_Address, size_t (Ring_Bytes), 3, 1 + 16#8000#, int (R.FD), 0);
         if Base = Sys.Map_Failed then
            return -Integer (Sys.Errno);
         end if;
         Entries_Base := Sys.Map (System.Null_Address, size_t (SQ_Count * 64), 3, 1 + 16#8000#,
                                  int (R.FD), 16#1000_0000#);
         if Entries_Base = Sys.Map_Failed then
            return -Integer (Sys.Errno);
         end if;
         R.SQ_Head := At_Offset (Base, Params (10));
         R.SQ_Tail := At_Offset (Base, Params (11));
         R.SQ_Mask := Load_32 (At_Offset (Base, Params (12)), Acquire);
         R.SQ_Entries := Load_32 (At_Offset (Base, Params (13)), Acquire);
         declare
            Index_Array : array (0 .. SQ_Count - 1) of Unsigned_32
              with Import, Address => At_Offset (Base, Params (16));
         begin
            -- Entry I always describes SQE I.
            for I in Index_Array'Range loop
               Index_Array (I) := Unsigned_32 (I);
            end loop;
         end;
         R.SQEs := Entries_Base;
         R.Tail := Load_32 (R.SQ_Tail, Acquire);
         R.CQ_Head := At_Offset (Base, Params (20));
         R.CQ_Tail := At_Offset (Base, Params (21));
         R.CQ_Mask := Load_32 (At_Offset (Base, Params (22)), Acquire);
         R.CQEs := At_Offset (Base, Params (25));
         R.Head := Load_32 (R.CQ_Head, Acquire);
      end;
      return 0;
   end Setup;

   type Resource_Register is record
      Count : Unsigned_32;
      Flags : Unsigned_32;
      Reserved : Unsigned_64;
      Data : Unsigned_64;
      Tags : Unsigned_64;
   end record with Convention => C;

   function Register_Sparse_Files (R : Ring; Count : Positive) return Integer is
      -- IORING_REGISTER_FILES2 with IORING_RSRC_REGISTER_SPARSE.
      Arg : aliased Resource_Register := (Unsigned_32 (Count), 1, 0, 0, 0);
      RC : constant long := Sys.Syscall (427, long (R.FD), 13, Sys.To_Long (Arg'Address), 32);
   begin
      return (if RC < 0 then -Integer (Sys.Errno) else 0);
   end Register_Sparse_Files;

   type Buffer_Register is record
      Ring_Address : Unsigned_64;
      Entries : Unsigned_32;
      Group : Unsigned_16;
      Flags : Unsigned_16;
      Reserved_1, Reserved_2, Reserved_3 : Unsigned_64;
   end record with Convention => C;

   function Setup_Buffers (R : in out Ring; Count : Positive; Size : Positive) return Integer is
      Ring_Bytes : constant Natural := (Count * 16 + 4095) / 4096 * 4096;
      Arg : aliased Buffer_Register;
      RC : long;
   begin
      -- MAP_PRIVATE | MAP_ANONYMOUS, page aligned as the kernel requires.
      R.Buffer_Ring := Sys.Map (System.Null_Address, size_t (Ring_Bytes), 3, 2 + 16#20#, -1, 0);
      if R.Buffer_Ring = Sys.Map_Failed then
         return -Integer (Sys.Errno);
      end if;
      R.Buffers := Sys.Map (System.Null_Address, size_t (Count * Size), 3, 2 + 16#20#, -1, 0);
      if R.Buffers = Sys.Map_Failed then
         return -Integer (Sys.Errno);
      end if;
      Arg := (To_U64 (R.Buffer_Ring), Unsigned_32 (Count), 0, 0, 0, 0, 0);
      -- IORING_REGISTER_PBUF_RING
      RC := Sys.Syscall (427, long (R.FD), 22, Sys.To_Long (Arg'Address), 1);
      if RC < 0 and then Sys.Errno = 22 then
         -- Ubuntu's 6.8.0-136 and later kernels invert the "reserved fields
         -- must be zero" check (memchr_inv used where mem_is_zero was meant),
         -- so they reject every valid registration. Such a kernel accepts the
         -- request with a nonzero reserved field; a correct kernel never gets
         -- here, because the first attempt succeeds.
         Arg.Reserved_1 := 1;
         RC := Sys.Syscall (427, long (R.FD), 22, Sys.To_Long (Arg'Address), 1);
      end if;
      if RC < 0 then
         return -Integer (Sys.Errno);
      end if;
      R.Buffer_Count := Unsigned_32 (Count);
      R.Buffer_Size := Size;
      R.Buffer_Tail := 0;
      for I in 0 .. Count - 1 loop
         Recycle (R, I);
      end loop;
      Publish_Buffers (R);
      return 0;
   end Setup_Buffers;

   function Buffer_Address (R : Ring; ID : Natural) return System.Address is
     (Sys.Offset (R.Buffers, ID * R.Buffer_Size));

   procedure Recycle (R : in out Ring; ID : Natural) is
      Item : constant System.Address :=
        Sys.Offset (R.Buffer_Ring, Natural (R.Buffer_Tail and (R.Buffer_Count - 1)) * 16);
      Address : Unsigned_64 with Import, Address => Item;
      Length : Unsigned_32 with Import, Address => Sys.Offset (Item, 8);
      Buffer_ID : Unsigned_16 with Import, Address => Sys.Offset (Item, 12);
   begin
      -- Bytes 14 .. 15 of the first entry hold the ring tail: never written here.
      Address := To_U64 (Buffer_Address (R, ID));
      Length := Unsigned_32 (R.Buffer_Size);
      Buffer_ID := Unsigned_16 (ID);
      R.Buffer_Tail := R.Buffer_Tail + 1;
      R.Buffers_Dirty := True;
   end Recycle;

   procedure Publish_Buffers (R : in out Ring) is
   begin
      if R.Buffers_Dirty then
         Store_16 (Sys.Offset (R.Buffer_Ring, 14), Unsigned_16 (R.Buffer_Tail and 16#FFFF#), Release);
         R.Buffers_Dirty := False;
      end if;
   end Publish_Buffers;

   function Get (R : in out Ring) return System.Address is
      Head : constant Unsigned_32 := Load_32 (R.SQ_Head, Acquire);
      Address : System.Address;
   begin
      if R.Tail - Head >= R.SQ_Entries then
         return System.Null_Address;
      end if;
      Address := Sys.Offset (R.SQEs, Natural (R.Tail and R.SQ_Mask) * 64);
      declare
         S : Submission with Import, Address => Address;
      begin
         S := Zero;
      end;
      R.Tail := R.Tail + 1;
      return Address;
   end Get;

   function Pending (R : Ring) return Unsigned_32 is
     (R.Tail - Load_32 (R.SQ_Head, Acquire));

   type Kernel_Time is record
      Seconds, Nanoseconds : Long_Long_Integer;
   end record with Convention => C;
   type Events_Arg is record
      Signal_Mask : Unsigned_64;
      Signal_Mask_Size : Unsigned_32;
      Padding : Unsigned_32;
      Timeout : Unsigned_64;
   end record with Convention => C;

   function Enter (R : in out Ring; Get_Events : Boolean; Wait_For : Natural := 0;
                   Timeout : Long_Long_Integer := -1) return Integer is
      Time : aliased Kernel_Time;
      Arg : aliased Events_Arg := (0, 0, 0, 0);
      To_Submit : Unsigned_32;
      RC : long;
   begin
      Store_32 (R.SQ_Tail, R.Tail, Release);
      To_Submit := R.Tail - Load_32 (R.SQ_Head, Acquire);
      if not Get_Events then
         if To_Submit = 0 then
            return 0;
         end if;
         RC := Sys.Syscall (426, long (R.FD), long (To_Submit), 0, 0, 0, 0);
      elsif Timeout >= 0 then
         Time := (Timeout / 1_000_000_000, Timeout mod 1_000_000_000);
         Arg.Timeout := To_U64 (Time'Address);
         -- IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG
         RC := Sys.Syscall (426, long (R.FD), long (To_Submit), long (Wait_For), 1 + 8,
                            Sys.To_Long (Arg'Address), 24);
      else
         RC := Sys.Syscall (426, long (R.FD), long (To_Submit), long (Wait_For), 1, 0, 0);
      end if;
      if RC < 0 then
         return -Integer (Sys.Errno);
      end if;
      return Integer (RC);
   end Enter;

   function Ready_Tail (R : Ring) return Unsigned_32 is (Load_32 (R.CQ_Tail, Acquire));

   procedure Read (R : Ring; Index : Unsigned_32; Item : out Completion) is
      C : Completion with Import, Address => Sys.Offset (R.CQEs, Natural (Index and R.CQ_Mask) * 16);
   begin
      Item := C;
   end Read;

   procedure Advance (R : in out Ring; To : Unsigned_32) is
   begin
      R.Head := To;
      Store_32 (R.CQ_Head, To, Release);
   end Advance;

   procedure Prepare_Accept (Entry_Address : System.Address; Listener : Integer;
                             User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Accept;
      S.FD := Integer_32 (Listener);
      S.Priority := 1;                    -- IORING_ACCEPT_MULTISHOT
      -- No accept4 flags: SOCK_CLOEXEC is invalid for direct descriptors.
      S.File_Index := 16#FFFF_FFFF#;      -- IORING_FILE_INDEX_ALLOC: direct slot
      S.User_Data := User_Data;
   end Prepare_Accept;

   procedure Prepare_Receive (Entry_Address : System.Address; Slot : Natural;
                              User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Receive;
      S.Flags := Fixed_File + Buffer_Select;
      S.FD := Integer_32 (Slot);
      S.Priority := 2;                    -- IORING_RECV_MULTISHOT
      S.Buffer_Group := 0;
      S.User_Data := User_Data;
   end Prepare_Receive;

   procedure Prepare_Send (Entry_Address : System.Address; Slot : Natural;
                           Data : System.Address; Length : Natural;
                           User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Send;
      S.Flags := Fixed_File;
      S.FD := Integer_32 (Slot);
      S.Address := To_U64 (Data);
      S.Length := Unsigned_32 (Length);
      S.Op_Flags := Sys.No_Signal;
      S.User_Data := User_Data;
   end Prepare_Send;

   procedure Prepare_Cancel_Slot (Entry_Address : System.Address; Slot : Natural;
                                  User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Cancel;
      S.FD := Integer_32 (Slot);
      -- IORING_ASYNC_CANCEL_ALL | _FD | _FD_FIXED
      S.Op_Flags := 1 + 2 + 8;
      S.User_Data := User_Data;
   end Prepare_Cancel_Slot;

   procedure Prepare_Cancel (Entry_Address : System.Address; Target : Unsigned_64;
                             User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Cancel;
      S.FD := -1;
      S.Address := Target;
      S.User_Data := User_Data;
   end Prepare_Cancel;

   procedure Prepare_Close (Entry_Address : System.Address; Slot : Natural;
                            User_Data : Unsigned_64) is
      S : Submission with Import, Address => Entry_Address;
   begin
      S.Opcode := Op_Close;
      S.File_Index := Unsigned_32 (Slot + 1);
      S.User_Data := User_Data;
   end Prepare_Close;
end Uring;
