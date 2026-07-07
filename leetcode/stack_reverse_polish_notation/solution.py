def eval_rpn(tokens: list[str]) -> int:
    operators = {
        "+": lambda a, b: a + b,
        "-": lambda a, b: a - b,
        "*": lambda a, b: a * b,
        # Truncate toward zero, unlike Python's floor division.
        "/": lambda a, b: int(a / b),
    }

    stack: list[int] = []
    for token in tokens:
        if token in operators:
            b = stack.pop()
            a = stack.pop()
            stack.append(operators[token](a, b))
        else:
            stack.append(int(token))

    return stack[0]


if __name__ == "__main__":
    print(eval_rpn(["2", "1", "+", "3", "*"]))
