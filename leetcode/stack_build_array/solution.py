def build_array(target: list[int], n: int) -> list[str]:
    """
    Build the target array using stack "Push"/"Pop" operations over the
    stream of integers 1..n.

    For each number in the stream we push it. If that number is not the next
    one needed for target, we immediately pop it back off. We stop as soon as
    the whole target has been placed on the stack.

    Args:
    target (list[int]): The strictly increasing target array.
    n (int): The stream contains the integers 1..n.

    Returns:
    list[str]: The sequence of "Push"/"Pop" operations.
    """
    operations: list[str] = []
    idx = 0  # index into target of the next value we need

    for value in range(1, n + 1):
        if idx == len(target):
            break

        operations.append("Push")
        if target[idx] == value:
            # Keep it: this is the next value we need.
            idx += 1
        else:
            # Not needed: discard it so the stream can continue.
            operations.append("Pop")

    return operations


if __name__ == "__main__":
    examples = [
        ([1, 3], 3),
        ([1, 2, 3], 3),
        ([1, 2], 4),
    ]
    for target, n in examples:
        result = build_array(target, n)
        print(f"target = {target}, n = {n} -> {result}")
