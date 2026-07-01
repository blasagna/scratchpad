import unittest

from solution import build_array


class TestBuildArray(unittest.TestCase):
    def test_example_1(self):
        self.assertEqual(build_array([1, 3], 3), ["Push", "Push", "Pop", "Push"])

    def test_example_2(self):
        self.assertEqual(build_array([1, 2, 3], 3), ["Push", "Push", "Push"])

    def test_example_3(self):
        # Stops as soon as target is built; never reads integer 3.
        self.assertEqual(build_array([1, 2], 4), ["Push", "Push"])

    def test_single_element_at_start(self):
        self.assertEqual(build_array([1], 5), ["Push"])

    def test_single_element_later(self):
        self.assertEqual(build_array([3], 3), ["Push", "Pop", "Push", "Pop", "Push"])

    def test_full_range(self):
        self.assertEqual(
            build_array([1, 2, 3, 4], 4),
            ["Push", "Push", "Push", "Push"],
        )

    def test_skips_between_values(self):
        self.assertEqual(
            build_array([2, 4], 4),
            ["Push", "Pop", "Push", "Push", "Pop", "Push"],
        )

    def test_stack_reconstructs_target(self):
        # Replaying the operations should yield the target array.
        target, n = [2, 3, 5], 6
        ops = build_array(target, n)
        stack: list[int] = []
        value = 1
        for op in ops:
            if op == "Push":
                stack.append(value)
                value += 1
            else:
                stack.pop()
        self.assertEqual(stack, target)


if __name__ == "__main__":
    unittest.main()
