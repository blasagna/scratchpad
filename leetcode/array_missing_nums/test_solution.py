import unittest

from solution import find_missing_nums


class TestFindMissingNums(unittest.TestCase):
    def test_example_with_duplicates(self):
        nums = [4, 3, 2, 7, 8, 2, 3, 1]
        self.assertEqual(sorted(find_missing_nums(nums)), [5, 6])

    def test_single_missing(self):
        nums = [1, 1]
        self.assertEqual(sorted(find_missing_nums(nums)), [2])

    def test_no_missing(self):
        nums = [1, 2, 3, 4]
        self.assertEqual(find_missing_nums(nums), [])

    def test_empty(self):
        self.assertEqual(find_missing_nums([]), [])

    def test_single_element_present(self):
        self.assertEqual(find_missing_nums([1]), [])

    def test_all_same_value(self):
        nums = [2, 2, 2]
        self.assertEqual(sorted(find_missing_nums(nums)), [1, 3])


if __name__ == "__main__":
    unittest.main()
