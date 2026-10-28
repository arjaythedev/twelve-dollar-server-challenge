package Workers is
   -- Fork before opening SQLite or creating sockets. Returns in each child;
   -- the foreground parent supervises them and fails if any child exits.
   procedure Start (Count : Positive);
   -- Serialize event batches within the shared one-CPU budget. This also
   -- prevents our own SQLite writers from waiting on one another.
   procedure Acquire;
   procedure Release;
end Workers;
