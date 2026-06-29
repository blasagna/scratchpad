import unittest
from solution import find_error_nums, find_error_nums_math, find_error_nums_xor

class TestFindErrorNumsOriginal(unittest.TestCase):
    def test_example_case(self):
        self.assertEqual(find_error_nums([1, 2, 2, 4]), (2, 3))

    def test_duplicate_at_start(self):
        self.assertEqual(find_error_nums([1, 1]), (1, 2))

    def test_duplicate_at_end(self):
        self.assertEqual(find_error_nums([2, 2]), (2, 1))

    def test_unsorted_input(self):
        self.assertEqual(find_error_nums([3, 2, 2]), (2, 1))
        self.assertEqual(find_error_nums([1, 5, 3, 2, 2]), (2, 4))

    def test_larger_input(self):
        nums = [1, 2, 3, 7, 5, 6, 7, 8, 9, 10]
        self.assertEqual(find_error_nums(nums), (7, 4))


class TestFindErrorNumsMath(unittest.TestCase):
    def test_example_case(self):
        self.assertEqual(find_error_nums_math([1, 2, 2, 4]), (2, 3))

    def test_duplicate_at_start(self):
        self.assertEqual(find_error_nums_math([1, 1]), (1, 2))

    def test_duplicate_at_end(self):
        self.assertEqual(find_error_nums_math([2, 2]), (2, 1))

    def test_unsorted_input(self):
        self.assertEqual(find_error_nums_math([3, 2, 2]), (2, 1))
        self.assertEqual(find_error_nums_math([1, 5, 3, 2, 2]), (2, 4))

    def test_larger_input(self):
        nums = [1, 2, 3, 7, 5, 6, 7, 8, 9, 10]
        self.assertEqual(find_error_nums_math(nums), (7, 4))


class TestFindErrorNumsXOR(unittest.TestCase):
    def test_example_case(self):
        self.assertEqual(find_error_nums_xor([1, 2, 2, 4]), (2, 3))

    def test_duplicate_at_start(self):
        self.assertEqual(find_error_nums_xor([1, 1]), (1, 2))

    def test_duplicate_at_end(self):
        self.assertEqual(find_error_nums_xor([2, 2]), (2, 1))

    def test_unsorted_input(self):
        self.assertEqual(find_error_nums_xor([3, 2, 2]), (2, 1))
        self.assertEqual(find_error_nums_xor([1, 5, 3, 2, 2]), (2, 4))

    def test_larger_input(self):
        nums = [1, 2, 3, 7, 5, 6, 7, 8, 9, 10]
        self.assertEqual(find_error_nums_xor(nums), (7, 4))


if __name__ == "__main__":
    unittest.main()
