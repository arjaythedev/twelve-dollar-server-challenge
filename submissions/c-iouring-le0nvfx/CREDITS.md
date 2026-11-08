# Source and design attribution

This is a continuation of le0nvfx's earlier C implementation from PR #352.
It does not replace that server with another submission's source.

- `src/btree_feed.h` adapts SQLite B-tree helper code from
  [c-iouring-pavlyhalim, PR #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245),
  at commit [`7387a313`](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/7387a313d58041c76e0fe8134825ef335eeafd0d/submissions/c-iouring-pavlyhalim).
  The C implementation's original author and contributors retain credit for
  that helper work.
- The ring-zero eventfd wait follows the published cross-ring wakeup design in
  #245. The event loop, connection lifecycle, HTTP handling, SQLite write path,
  and WAL VFS here remain this submission's implementation.
- The per-request HS256 key-state approach follows the published design in
  [rust-iouring-kensac, PR #264](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264);
  this submission implements it with OpenSSL EVP contexts.

The helper reuse is covered by the challenge repository's MIT license; see
[LICENSE](LICENSE). The official 295,000-user score belongs to PR #245 and is
not claimed for this entry.
