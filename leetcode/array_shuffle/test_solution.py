import unittest
from solution import shuffle


class TestShuffle(unittest.TestCase):
    def test_shuffle_basic(self):
        self.assertEqual(shuffle([2, 5, 1, 3, 4, 7], 3), [2, 3, 5, 4, 1, 7])

    def test_shuffle_another(self):
        self.assertEqual(shuffle([1, 2, 3, 4, 4, 3, 2, 1], 4), [1, 4, 2, 3, 3, 2, 4, 1])

    def test_shuffle_simple(self):
        self.assertEqual(shuffle([1, 1, 2, 2], 2), [1, 2, 1, 2])

    def test_shuffle_empty(self):
        self.assertEqual(shuffle([], 0), [])


if __name__ == "__main__":
    unittest.main()
