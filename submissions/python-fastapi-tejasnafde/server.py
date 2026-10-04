"""Starts the app with uvicorn: one process, uvloop event loop, httptools HTTP parser."""
import os
import resource

import uvicorn

if __name__ == "__main__":
    # Each keep-alive client is one open socket. Ubuntu's default soft limit is 1024, so raise it
    # to the hard limit inside the process (rule 11 allows in-process settings).
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    uvicorn.run(
        "app:app",
        host=os.environ.get("HOST", "127.0.0.1"),
        port=int(os.environ.get("PORT", "3000")),
        loop="uvloop",
        http="httptools",
        lifespan="off",
        access_log=False,
        proxy_headers=False,  # no proxy in front: skip uvicorn's X-Forwarded-* middleware
        log_level="warning",
        server_header=False,
        timeout_keep_alive=75,  # SPEC: keep idle connections open for at least 65 s
        backlog=4096,
    )
