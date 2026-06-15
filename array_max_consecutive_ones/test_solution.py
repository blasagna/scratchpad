import unittest
from solution import find_max_consecutive_ones

class TestFindMaxConsecutiveOnes(unittest.TestCase):
    def test_empty_list(self):
        self.assertEqual(find_max_consecutive_ones([]), 0)

    def test_only_zeros(self):
        self.assertEqual(find_max_consecutive_ones([0, 0, 0]), 0)

    def test_only_ones(self):
        self.assertEqual(find_max_consecutive_ones([1, 1, 1, 1]), 4)

    def test_max_in_middle(self):
        self.assertEqual(find_max_consecutive_ones([1, 0, 1, 1, 0, 1]), 2)

    def test_max_at_beginning(self):
        self.assertEqual(find_max_consecutive_ones([1, 1, 1, 0, 1, 1]), 3)

    def test_max_at_end(self):
        self.assertEqual(find_max_consecutive_ones([1, 1, 0, 1, 1, 1]), 3)

    def test_alternating(self):
        self.assertEqual(find_max_consecutive_ones([1, 0, 1, 0, 1, 0, 1]), 1)

    def test_single_zero(self):
        self.assertEqual(find_max_consecutive_ones([0]), 0)

    def test_single_one(self):
        self.assertEqual(find_max_consecutive_ones([1]), 1)

if __name__ == "__main__":
    unittest.main()
