import unittest
from solution import exclusive_time


class TestExclusiveTime(unittest.TestCase):
    def test_example_1(self):
        self.assertEqual(
            exclusive_time(2, ["0:start:0", "1:start:2", "1:end:5", "0:end:6"]),
            [3, 4],
        )

    def test_example_2_recursive(self):
        self.assertEqual(
            exclusive_time(
                1,
                ["0:start:0", "0:start:2", "0:end:5", "0:start:6", "0:end:6", "0:end:7"],
            ),
            [8],
        )

    def test_example_3_mixed(self):
        self.assertEqual(
            exclusive_time(
                2,
                ["0:start:0", "0:start:2", "0:end:5", "1:start:6", "1:end:6", "0:end:7"],
            ),
            [7, 1],
        )

    def test_single_call(self):
        self.assertEqual(exclusive_time(1, ["0:start:0", "0:end:0"]), [1])

    def test_no_nesting(self):
        self.assertEqual(
            exclusive_time(2, ["0:start:0", "0:end:1", "1:start:2", "1:end:3"]),
            [2, 2],
        )


if __name__ == "__main__":
    unittest.main()
