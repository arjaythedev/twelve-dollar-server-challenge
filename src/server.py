"""Run the reference API directly on HOST:PORT."""

import os
from pathlib import Path

from granian import Granian
from granian.constants import HTTPModes, Interfaces, Loops, RuntimeModes
from granian.http import HTTP1Settings


def main() -> None:
    os.environ["SQLITE_PATH"] = str(Path(os.environ["SQLITE_PATH"]).resolve())
    Granian(
        "api.app:create_application",
        factory=True,
        working_dir=Path(__file__).resolve().parent,
        address=os.environ.get("HOST", "127.0.0.1"),
        port=int(os.environ.get("PORT", "3000")),
        interface=Interfaces.RSGI,
        http=HTTPModes.http1,
        loop=Loops.asyncio,
        runtime_mode=RuntimeModes.st,
        workers=1,
        runtime_threads=1,
        websockets=False,
        backlog=4096,
        backpressure=65536,
        http1_settings=HTTP1Settings(header_read_timeout=75_000),
        log_access=False,
    ).serve()


if __name__ == "__main__":
    main()
