"""Starts the app with uvicorn: one process, uvloop event loop, httptools HTTP parser."""
import os

import uvicorn

if __name__ == "__main__":
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
