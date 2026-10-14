import unittest

from adapters.descriptor_budget import DescriptorBudget, descriptor_is_open


class DescriptorBudgetTest(unittest.IsolatedAsyncioTestCase):
    async def test_strained_while_budget_descriptor_open(self):
        open_descriptors = set()
        budget = DescriptorBudget(budget=3, interval_seconds=3600, is_open=open_descriptors.__contains__)
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


if __name__ == "__main__":
    unittest.main()
