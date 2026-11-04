"""Project test suites"""

import tempfile
import time


class TemporaryDirectory(tempfile.TemporaryDirectory[str]):
    def cleanup(self) -> None:
        for attempt in range(5):
            try:
                super().cleanup()
                return
            except OSError as error:
                if getattr(error, "winerror", None) != 145 or attempt == 4:
                    raise
                time.sleep(0.05 * (attempt + 1))
