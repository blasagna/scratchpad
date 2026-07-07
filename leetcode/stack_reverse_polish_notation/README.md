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

## complexity

The stack-based solution runs in **O(n) time** and **O(n) space**, where `n` is the number of tokens.

- **Time:** Each token is processed exactly once. Operands do a single `int()` conversion and push; operators pop two values, apply one arithmetic op, and push the result. All of these are O(1) (the values fit in a 32-bit integer per the constraints), so the total is linear.
- **Space:** The stack is the only structure that grows with the input. In the worst case a long run of operands is pushed before any operator appears (e.g. `["1","1",...,"+","+",...]`), so the stack can hold up to ~n/2 values at once — O(n).

### can the space be improved?

The **asymptotic worst case cannot be beaten**: stack depth equals the depth of the expression tree, and a valid input can force ~n/2 pending operands to be held simultaneously before any can be combined.

However, the *auxiliary* space can be reduced to **O(1)** by reusing the input list as the stack via a write pointer instead of allocating a separate stack:

```python
def eval_rpn(tokens: list[str]) -> int:
    operators = {
        "+": lambda a, b: a + b,
        "-": lambda a, b: a - b,
        "*": lambda a, b: a * b,
        "/": lambda a, b: int(a / b),
    }
    top = 0  # index one past the stack's top, within tokens itself
    for token in tokens:
        if token in operators:
            b, a = tokens[top - 1], tokens[top - 2]
            top -= 1
            tokens[top - 1] = operators[token](a, b)
        else:
            tokens[top] = int(token)
            top += 1
    return tokens[0]
```

This is safe because after reading `k` tokens the stack size is `operands − operators ≤ operands + operators = k`, so the write pointer never passes the read position. The tradeoff is that it mutates the caller's list and stores mixed types (`int` over `str`), so the current implementation keeps the separate, more readable stack.
