import tempfile
import unittest
from pathlib import Path

from adapters.fd_budget import FdBudget


class FdBudgetTest(unittest.IsolatedAsyncioTestCase):
    async def test_strained_once_over_budget(self):
        with tempfile.TemporaryDirectory() as fds:
            budget = FdBudget(budget=3, every_s=3600, fd_dir=fds)
            for n in range(3):
                Path(fds, str(n)).touch()
            await budget.start()
            self.assertFalse(budget.strained)
            Path(fds, "3").touch()
            budget._sample()
            self.assertTrue(budget.strained)
            await budget.close()

    async def test_disabled_without_proc(self):
        budget = FdBudget(budget=0, fd_dir="/nonexistent/fd")
        await budget.start()
        self.assertFalse(budget.strained)
        await budget.close()


if __name__ == "__main__":
    unittest.main()
