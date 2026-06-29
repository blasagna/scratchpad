import unittest
from solution import (
    smaller_numbers_than_current,
    smaller_numbers_than_current_sorted,
    smaller_numbers_than_current_counting,
)

class TestSmallerNumbersThanCurrent(unittest.TestCase):
    def assert_all_implementations(self, nums, expected):
        for name, func in [
            ("default", smaller_numbers_than_current),
            ("sorted", smaller_numbers_than_current_sorted),
            ("counting", smaller_numbers_than_current_counting),
        ]:
            with self.subTest(implementation=name):
                self.assertEqual(func(nums), expected)

    def test_example_case(self):
        self.assert_all_implementations([8, 1, 2, 2, 3], [4, 0, 1, 1, 3])

    def test_all_same_elements(self):
        self.assert_all_implementations([7, 7, 7, 7], [0, 0, 0, 0])

    def test_increasing_array(self):
        self.assert_all_implementations([1, 2, 3, 4], [0, 1, 2, 3])

    def test_decreasing_array(self):
        self.assert_all_implementations([4, 3, 2, 1], [3, 2, 1, 0])

    def test_empty_array(self):
        self.assert_all_implementations([], [])

    def test_single_element(self):
        self.assert_all_implementations([1], [0])

    def test_negative_numbers(self):
        self.assert_all_implementations([-1, -2, 0, 2], [1, 0, 2, 3])

if __name__ == "__main__":
    unittest.main()
