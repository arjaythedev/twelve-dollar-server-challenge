-- The event loop's SQLite connection (the checkpoint helper has its own).
-- Requests handled in the same event batch share one transaction, opened on
-- first use and committed by Finish_Batch before any of their responses are
-- sent.
package Database is
   Database_Error : exception;
   procedure Open (Path : String);
   procedure Close;

   function In_Transaction return Boolean;

   -- While True, write requests must wait (they would keep the WAL from
   -- restarting); reads go on. See the body.
   function Writes_Held return Boolean;
   -- True once, when held writes may run: start them before any other
   -- request of the next batch.
   function Release_Writes return Boolean;
   -- Counts since the last call, for the periodic stats line.
   function Checkpoint_Stats return String;
   -- Commit the batch. False means its writes did not commit.
   function Finish_Batch return Boolean;
   -- Reset statements after a failed request; notes a lost transaction.
   procedure Recover;
   function Last_Error return String;

   -- The query procedures write response JSON with Output.
   procedure Check;
   procedure Feed;
   function Post (ID : Long_Long_Integer) return Boolean;
   procedure Create_Post (User_ID : Long_Long_Integer; Text, Username : String);
   procedure Like (User_ID, Post_ID : Long_Long_Integer; Found, Already : out Boolean);
end Database;
