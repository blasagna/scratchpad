import unittest
from solution import smaller_numbers_than_current

class TestSmallerNumbersThanCurrent(unittest.TestCase):
    def test_example_case(self):
        self.assertEqual(smaller_numbers_than_current([8, 1, 2, 2, 3]), [4, 0, 1, 1, 3])

    def test_all_same_elements(self):
        self.assertEqual(smaller_numbers_than_current([7, 7, 7, 7]), [0, 0, 0, 0])

    def test_increasing_array(self):
        self.assertEqual(smaller_numbers_than_current([1, 2, 3, 4]), [0, 1, 2, 3])

    def test_decreasing_array(self):
        self.assertEqual(smaller_numbers_than_current([4, 3, 2, 1]), [3, 2, 1, 0])

    def test_empty_array(self):
        self.assertEqual(smaller_numbers_than_current([]), [])

    def test_single_element(self):
        self.assertEqual(smaller_numbers_than_current([1]), [0])

    def test_negative_numbers(self):
        self.assertEqual(smaller_numbers_than_current([-1, -2, 0, 2]), [1, 0, 2, 3])

if __name__ == "__main__":
    unittest.main()
