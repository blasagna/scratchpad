import unittest
from solution import find_error_nums

class TestFindErrorNums(unittest.TestCase):
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
        # 1 to 10, duplicate 7, missing 4
        nums = [1, 2, 3, 7, 5, 6, 7, 8, 9, 10]
        self.assertEqual(find_error_nums(nums), (7, 4))

if __name__ == "__main__":
    unittest.main()
