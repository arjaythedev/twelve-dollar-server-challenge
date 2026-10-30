with Interfaces;

-- WAL checkpoints from a helper process.
--
-- A checkpoint copies committed pages from the WAL into the database file
-- and syncs both files. Run inside a commit, that copy and those syncs
-- stall the event loop. The helper does the work with its own connection,
-- every Interval and whenever the server asks for a pass. The server then
-- only has to let the WAL restart (see Database), which costs one small
-- header sync instead of a whole checkpoint.
package Checkpointer is
   -- Fork the helper. Call before this process opens any SQLite connection:
   -- SQLite connections must not cross fork().
   procedure Start (Path : String);

   -- True while the helper is running and responsive.
   function Available return Boolean;

   -- Ask for a pass that starts after this call; returns its ticket.
   function Request_Pass return Interfaces.Unsigned_32;

   -- True once a pass for Ticket (or a later one) has finished and left
   -- every WAL frame copied into the database file.
   function Caught_Up (Ticket : Interfaces.Unsigned_32) return Boolean;
end Checkpointer;
