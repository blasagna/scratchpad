# evaluate reverse polish notation

## problem statement

You are given an array of strings tokens that represents an arithmetic expression in a [Reverse Polish Notation](https://en.wikipedia.org/wiki/Reverse_Polish_notation).

Evaluate the expression. Return an integer that represents the value of the expression.

Note that:

1. The valid operators are '+', '-', '*', and '/'.
1. Each operand may be an integer or another expression.
1. The division between two integers always truncates toward zero.
1. There will not be any division by zero.
1. The input represents a valid arithmetic expression in a reverse polish notation.
1. The answer and all the intermediate calculations can be represented in a 32-bit integer.
 

## examples

Example 1:

Input: tokens = ["2","1","+","3","*"]
Output: 9
Explanation: ((2 + 1) * 3) = 9
Example 2:

Input: tokens = ["4","13","5","/","+"]
Output: 6
Explanation: (4 + (13 / 5)) = 6
Example 3:

Input: tokens = ["10","6","9","3","+","-11","*","/","*","17","+","5","+"]
Output: 22
Explanation: ((10 * (6 / ((9 + 3) * -11))) + 17) + 5
= ((10 * (6 / (12 * -11))) + 17) + 5
= ((10 * (6 / -132)) + 17) + 5
= ((10 * 0) + 17) + 5
= (0 + 17) + 5
= 17 + 5
= 22
 

## Constraints:

1. 1 <= tokens.length <= 104
1. tokens[i] is either an operator: "+", "-", "*", or "/", or an integer in the range [-200, 200].
