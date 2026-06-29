import unittest
from typing import List # Although not strictly necessary for the test file, good practice if type hinting internally
# Assuming solution.py is in the same directory and contains 'get_concatenation'
from solution import get_concatenation

class TestGetConcatenation(unittest.TestCase):

    def test_basic_integer_list(self):
        """Tests concatenation with a standard, non-empty list of integers."""
        original = [1, 2, 3]
        expected = [1, 2, 3, 1, 2, 3]
        result = get_concatenation(original)
        self.assertEqual(result, expected)

    def test_empty_list(self):
        """Tests concatenation when provided with an empty list."""
        original: List[int] = []
        expected: List[int] = []
        result = get_concatenation(original)
        # Both the input and output must be lists of integers (empty list satisfies this).
        self.assertEqual(result, expected)

    def test_single_element_list(self):
        """Tests concatenation with a list containing only one integer element."""
        original = [42]
        expected = [42, 42]
        result = get_concatenation(original)
        self.assertEqual(result, expected)

    def test_multiple_elements(self):
        """Tests concatenation with a larger set of integer elements."""
        original = [10, 20, -5]
        expected = [10, 20, -5, 10, 20, -5]
        result = get_concatenation(original)
        self.assertEqual(result, expected)

# This allows running the tests directly from the command line using 'python test_solutions.py'
if __name__ == '__main__':
    unittest.main()
