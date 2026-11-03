-- The single-threaded event loop. Both backends share connection state,
-- request processing and the batch rule: responses produced inside a batch
-- transaction are sent only after that transaction commits.
package Engine is
   -- io_uring with direct (fixed-table) sockets: several rings of up to
   -- 65,535 sockets each, beyond the per-process descriptor limit. Returns
   -- with Available = False when the kernel or a sandbox refuses io_uring.
   procedure Run_Uring (Listener : Integer; Available : out Boolean);
   -- Portable fallback: one epoll loop over ordinary descriptors.
   procedure Run_Epoll (Listener : Integer);
end Engine;
