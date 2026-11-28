//! The event loop: io_uring setup (multishot accept/recv, provided buffers),
//! batched passes, group-commit integration point for the server.

// TODO(challenge): ring setup with resv[0]=1 retry; pass = one SQLite
// transaction; responses flush only after COMMIT.
