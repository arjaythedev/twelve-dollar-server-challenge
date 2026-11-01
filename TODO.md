# TODO

- [x] Check the rules and study other submissions, including the record's history.
- [x] Build the typed Python/Granian baseline with uv.
- [x] Pass the official checks and extra tests on fresh data.
- [x] Record the first Docker benchmarks.
- [x] Reduce the docs and combine dev tooling.
- [x] Rename the package to api and upgrade to current stable Python.
- [x] Profile the unchanged request path at 5,000 users: SQL, JSON, auth and remaining work.
- [x] Compare stdlib, orjson, msgspec, SQLite JSON and a native JSON writer.
- [x] Encode feed and post reads directly in C through SQLite's extension API.
- [ ] Compare changes on the same setup and repeat runs.
- [ ] Later: test capacity on a comparable Linux VM with a separate load generator.
- [ ] Finally: move our src contents and docs into `submissions/<stack>-SmolBlackHole/` with git mv. Rename SUBMISSION.md to README.md, update paths and rerun checks. Exclude generated output.

Keep our code in src/, use English and strict typing. Leave upstream files and other submissions alone.

Dev commands: `uv run --directory src --locked dev.py --help`.
