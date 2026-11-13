"""A Resource that profiles the server from startup to shutdown with cProfile."""

import cProfile
from pathlib import Path


class CProfileRecorder:
    __slots__ = ("_out", "_profile")

    def __init__(self, out_path: str):
        self._out = Path(out_path)
        self._profile = cProfile.Profile()

    async def start(self) -> None:
        self._profile.enable()

    async def close(self) -> None:
        self._profile.disable()
        self._out.parent.mkdir(parents=True, exist_ok=True)
        self._profile.dump_stats(self._out)
