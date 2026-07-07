import unittest
from solution import eval_rpn


class TestEvalRpn(unittest.TestCase):
    def test_add_then_multiply(self):
        self.assertEqual(eval_rpn(["2", "1", "+", "3", "*"]), 9)

    def test_division_truncates(self):
        self.assertEqual(eval_rpn(["4", "13", "5", "/", "+"]), 6)

    def test_complex_expression(self):
        tokens = ["10", "6", "9", "3", "+", "-11", "*", "/", "*", "17", "+", "5", "+"]
        self.assertEqual(eval_rpn(tokens), 22)

    def test_single_operand(self):
        self.assertEqual(eval_rpn(["42"]), 42)

    def test_negative_operand(self):
        self.assertEqual(eval_rpn(["-3"]), -3)

    def test_division_truncates_toward_zero(self):
        # -7 / 2 truncates to -3, not floor of -4.
        self.assertEqual(eval_rpn(["-7", "2", "/"]), -3)

    def test_subtraction_order(self):
        self.assertEqual(eval_rpn(["5", "3", "-"]), 2)


if __name__ == "__main__":
    unittest.main()
