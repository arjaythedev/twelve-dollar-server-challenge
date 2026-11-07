# Python + FastAPI

| | |
|---|---|
| Language | Python PyPy 7.3.15 (like CPython 3.9.18) (Ubuntu 24.04's `pypy3` package) |
| Framework | Twisted (latest) |
| SQLite driver | the standard library `sqlite3` module, linked to Ubuntu's SQLite 3.45.1 |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:3000` itself (or as specified via HOST and PORT) |

All dependency versions, transitive ones included, are pinned in `requirements.txt`.

## Running it

```bash
sudo bash install.sh   # apt: pypy3 python3-venv
bash build.sh          # creates .venv and installs requirements.txt
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why
- **Only One Resource**
  The API endpoints are not composable, and the deepest is 3 levels.  So we don't need any of twisted's advanced routing options.
  Instead, we intercept each request at the top level render function; right after the headers and body are read, and the path is split
  into segments.

- **If blocks**
  The routing *should* be a match statement, but ubuntu only has an old version of pypy, which is at python3.9 syntax.  So it's forced to
  use a set of if/else blocks.  It could use a dict, but the hash function is *slow*, so dicts are best avoided, especially with string keys.

- **Lists, lists, and more lists**
  On that note, the longest list we can expect to see is 200 items deep (with the default pypy tuning).  Anything shorter than 50, linear search wins by a wide margin.
  Since processing the list is destructive, it gets shorter as it goes.  Also, sqlite *usually* gives us the answers back in roughly the right order, so the search time
  is almost O(1).  This is the current performance bottleneck, especially if you run this with CPython, as much past 35k users (or about half that with CPython), the
  reconciliation phase gets too expensive.

- **Batching**
  The RAII wrapper for the database queues all access.  Even the /health endpoint has to go through the queue.  It also deduplicates pending requests, without caching any
  results.  This is the inversion-of-control that has let twisted excel in high performance applications for 20 years.  In this case, we're just hanging onto the requests,
  rather than paying the overhead in wrapping them behind deferreds, since once again, this API is simple and flat.

- **Templates, not json**
  "In general, an implementation should be conservative in its sending behavior, and liberal in its receiving behavior." -- J. Postel.
  We aren't sending arbitrary objects serialized over the wire.  There are only a small number of possible messages.  So calling a general purpose json serializer is a poor
  choice.  While *we* know the structure of the data, it's a tall order for the JIT to figure it out if we just pass it off to json.dumps.  So instead we use *another* snippet
  format, this time in the form of python's lambdas.  Calling .format, or using fstrings, or the old fashioned %() encoding are all slower, at least on this old version of pypy.
  It's at least 2x slower than fixed string concatenation; again, be nice to the JIT.  On the other hand, we can't rely on the user giving us fixed-format json, so we'll let
  the parser library deal with that mess.

- **Exceptions**
  Table based exceptions are fantastic.  In the happy path, they have 0 runtime overhead, and for the most part we'll be happy to just dump the sad path.  We don't have table
  based exceptions, so we'd best avoid them.  This probably means it would be worth writing a minimal json parser via any other means, but it turns out pypy is *pretty good* at
  optimizing away exception overhead.  If this were written for CPython, it would be worth dealing with them.  In the mean time, we don't raise any exceptions of our own.

- **Nursery**
  pypy has a very good gc, and we're super JIT friendly which elides some of the garbage, but this machine has oodles of ram, so we'll give it a bigger nursery (8MB) to avoid gc
  pauses during spikes.  It'd probably be possible to further tune it to avoid gc running while batch processing, but the returns are likely to be small

- **Batch Delay**
  The latency is tuned for about 150ms of latency at idle, average.  In practice it's closer to 200-300.  As the work queues fill, they'll trip due to load rather than time,
  and the latency will drop until the CPU usage starts to average in the upper 80s.  In theory, if the server could estimate the number of active users, it could tune this
  latency to match its throughput a little better, but it only matters at fairly low user counts.

- **Bulk Calls**
  pypy's FFI layer has improved in the past 10 years, but it is still *slow*.  The fewer times we cross the FFI boundry, the better.  So we avoid manually starting transactions
  and then making sequential queries.  Instead, we build bulk queries, for both reads and writes, using pregenerated query strings.  The issue is sqlite doesn't guarantee the
  returned new entries will match the submission order, and in the case of likes, it silently omits any which already existed.  So we have to align the results with the queries,
  which uses a linear search since the batch size is fairly modest and *hashing is slow*.  It helps that sqlite *usually* gives the results in the order you want.

## License

MIT, under the repo's [license](../../LICENSE).
