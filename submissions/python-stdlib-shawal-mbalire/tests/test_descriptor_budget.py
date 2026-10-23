import os
import tempfile
import unittest

from adapters.descriptor_budget import DescriptorBudget, SocketCounter, descriptor_is_open, tcp_sockets_in_use

SOCKSTAT = (b"sockets: used 62301\n"
            b"TCP: inuse 61450 orphan 18 tw 32768 alloc 61470 mem 9000\n"
            b"UDP: inuse 0 mem 0\n")


class DescriptorBudgetTest(unittest.IsolatedAsyncioTestCase):
    async def test_strained_while_at_budget(self):
        open_descriptors = set()
        budget = DescriptorBudget(budget=3, interval_seconds=3600, at_budget=open_descriptors.__contains__)
        open_descriptors.update({0, 1, 2})
        await budget.start()
        self.assertFalse(budget.strained)
        open_descriptors.add(3)
        budget._sample()
        self.assertTrue(budget.strained)
        open_descriptors.discard(3)
        budget._sample()
        self.assertFalse(budget.strained)
        await budget.close()

    def test_descriptor_is_open(self):
        self.assertTrue(descriptor_is_open(0))
        self.assertFalse(descriptor_is_open(1 << 20))

    def test_tcp_sockets_in_use(self):
        self.assertEqual(tcp_sockets_in_use(SOCKSTAT), 61450)

    def test_socket_counter_rereads_the_file(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(SOCKSTAT)
        try:
            at_budget = SocketCounter(f.name)
            self.assertTrue(at_budget(61450))
            self.assertFalse(at_budget(61451))
            with open(f.name, "wb") as rewritten:
                rewritten.write(SOCKSTAT.replace(b"inuse 61450", b"inuse 70000"))
            self.assertTrue(at_budget(61451))
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
