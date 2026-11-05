# Rules check, objc-uring-delneg (5 Nov). Read upstream README rules, SPEC.md, LEADERBOARD.md notes and flags at `fe19e96`.

| trap | where it says so | status |
|---|---|---|
| Rule 2 stock SQLite, no patch | LEADERBOARD "Arjay's rulings on SQLite (2026-10-31)"; #154, #158 pulled until dropped | clear: build.sh uses the amalgamation unedited; td_btree.c only includes it |
| No own engine for the file format | same note, #210 | clear: reads go through `sqlite3Btree*` cursors |
| No writing SQLite private state (ruling 8) | flags on #296, #317, #318 (`BtCursor` fields) | to verify before the PR: td_btree.c must only call functions, never set fields |
| Rule 3 schema | README rule 3 | clear: no DDL |
| Rule 5 no cross-request cache; JWT verified each request | README rule 5; #260, #243, #277 coalescing held open | clear: no coalescing; SQLite's own page cache allowed by name, so a bigger cache_size is fine |
| Rule 6 durable before 201 | README rule 6 | clear: WAL + NORMAL, response submitted after COMMIT |
| Rule 7 no load-test detection | README rule 7; #32 evict-order flag; #112 sheds on /proc/meminfo | clear: nothing reads /proc/meminfo or keys on traffic shape |
| Rule 10 pinned, built on the box, healthy in 60 s; 1 h install timeout (#191, #143, #166) | README rule 10; LB | install.sh 40 s and PGO build ~2 min on 8 vCPU Synteq; clean `ubuntu:24.04` on 1 vCPU still to run before the PR |
| Rule 11 no kernel parameters, no pinning | README rule 11; #139 flag acts outside its folder | clear so far: start.sh only execs the binary; setrlimit and per-socket options are own-process. mlock within RLIMIT_MEMLOCK and setpriority on our own child are own-process settings (#245, #257 do both, unflagged) |
| Touch only own folder | README "Submitting"; #139 | PR must diff only `submissions/objc-uring-delneg/` |
| Copies | flags on #291, #303, #314, #271; Arjay 2026-11-04: credited reuse of helpers is fine | own code only; ideas from #245, #264, #257 credited in README |
| start.sh restart loop | flag: Deno #119, c-epoll #64, raoz #26, Antheurus #65 | clear: `exec bin/server` |
| Tokens without `exp` | flag list | clear: `exp` required |
| Follow-up merges only if it beats the merged row | #319, #278, #293 comments | a PR must score above 280,000 to merge; head must not move during a run (#304 lost its merge that way): don't push while it's being scored |
