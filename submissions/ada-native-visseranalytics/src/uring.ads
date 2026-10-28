with Interfaces; use Interfaces;
with System;

-- A minimal io_uring binding written against the kernel ABI (no liburing):
-- ring setup, submission/completion queues, sparse fixed-file tables and a
-- provided-buffer ring for multishot receives.
package Uring is
   type Ring is record
      FD : Integer := -1;
      Deferred : Boolean := False;   -- IORING_SETUP_DEFER_TASKRUN in use
      SQ_Head, SQ_Tail : System.Address := System.Null_Address;
      SQ_Mask, SQ_Entries : Unsigned_32 := 0;
      SQEs : System.Address := System.Null_Address;
      Tail : Unsigned_32 := 0;        -- next SQE to fill
      CQ_Head, CQ_Tail : System.Address := System.Null_Address;
      CQ_Mask : Unsigned_32 := 0;
      CQEs : System.Address := System.Null_Address;
      Head : Unsigned_32 := 0;        -- next CQE to read
      -- Provided receive buffers (buffer group 0).
      Buffer_Ring : System.Address := System.Null_Address;
      Buffers : System.Address := System.Null_Address;
      Buffer_Count : Unsigned_32 := 0;
      Buffer_Size : Natural := 0;
      Buffer_Tail : Unsigned_32 := 0;
      Buffers_Dirty : Boolean := False;
   end record;

   type Completion is record
      User_Data : Unsigned_64;
      Result : Integer_32;
      Flags : Unsigned_32;
   end record with Convention => C;

   F_Buffer : constant := 1;
   F_More : constant := 2;

   -- Returns 0 or a negative errno.
   function Setup (R : in out Ring; Entries, Completions : Positive) return Integer;
   function Register_Sparse_Files (R : Ring; Count : Positive) return Integer;
   function Setup_Buffers (R : in out Ring; Count : Positive; Size : Positive) return Integer;

   -- A zeroed submission entry, or Null_Address when the queue is full.
   function Get (R : in out Ring) return System.Address;
   -- io_uring_enter: submits everything queued, optionally waiting for
   -- Wait_For completions (and running deferred task work) up to Timeout
   -- nanoseconds (negative: no timeout). Returns the syscall result.
   function Enter (R : in out Ring; Get_Events : Boolean; Wait_For : Natural := 0;
                   Timeout : Long_Long_Integer := -1) return Integer;
   function Pending (R : Ring) return Unsigned_32;

   function Ready_Tail (R : Ring) return Unsigned_32 with Inline;
   procedure Read (R : Ring; Index : Unsigned_32; Item : out Completion) with Inline;
   procedure Advance (R : in out Ring; To : Unsigned_32) with Inline;

   function Buffer_Address (R : Ring; ID : Natural) return System.Address with Inline;
   procedure Recycle (R : in out Ring; ID : Natural);
   procedure Publish_Buffers (R : in out Ring);

   procedure Prepare_Accept (Entry_Address : System.Address; Listener : Integer;
                             User_Data : Unsigned_64);
   procedure Prepare_Receive (Entry_Address : System.Address; Slot : Natural;
                              User_Data : Unsigned_64);
   procedure Prepare_Send (Entry_Address : System.Address; Slot : Natural;
                           Data : System.Address; Length : Natural;
                           User_Data : Unsigned_64);
   procedure Prepare_Cancel_Slot (Entry_Address : System.Address; Slot : Natural;
                                  User_Data : Unsigned_64);
   procedure Prepare_Cancel (Entry_Address : System.Address; Target : Unsigned_64;
                             User_Data : Unsigned_64);
   procedure Prepare_Close (Entry_Address : System.Address; Slot : Natural;
                            User_Data : Unsigned_64);
end Uring;
