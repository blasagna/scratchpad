"""Stack implementations: array-backed vs. linked-list-backed.

A stack is a LIFO (last-in, first-out) collection supporting:
    push(x) - add x to the top
    pop()   - remove and return the top item
    peek()  - return the top item without removing it
    is_empty() / len()

Below are two implementations with the same interface, followed by a
discussion of the tradeoffs and a demonstration.
"""

from __future__ import annotations

from typing import Generic, Optional, TypeVar

T = TypeVar("T")


class StackEmptyError(Exception):
    """Raised when pop/peek is called on an empty stack."""


# ---------------------------------------------------------------------------
# 1. Array-backed stack
# ---------------------------------------------------------------------------
class ArrayStack(Generic[T]):
    """Stack backed by a dynamic array (Python list).

    The top of the stack is the *end* of the list, so push/pop map to
    ``list.append`` / ``list.pop`` — both amortized O(1).
    """

    def __init__(self) -> None:
        self._items: list[T] = []

    def push(self, item: T) -> None:
        self._items.append(item)  # amortized O(1)

    def pop(self) -> T:
        if not self._items:
            raise StackEmptyError("pop from empty stack")
        return self._items.pop()  # O(1) from the end

    def peek(self) -> T:
        if not self._items:
            raise StackEmptyError("peek at empty stack")
        return self._items[-1]

    def is_empty(self) -> bool:
        return not self._items

    def __len__(self) -> int:
        return len(self._items)

    def __repr__(self) -> str:
        # left = bottom, right = top
        return f"ArrayStack(bottom -> top: {self._items})"


# ---------------------------------------------------------------------------
# 2. Linked-list-backed stack
# ---------------------------------------------------------------------------
class _Node(Generic[T]):
    """Singly-linked list node. `next` points toward the bottom of the stack."""

    __slots__ = ("value", "next")

    def __init__(self, value: T, next: Optional["_Node[T]"] = None) -> None:
        self.value = value
        self.next = next


class LinkedStack(Generic[T]):
    """Stack backed by a singly linked list.

    The `_head` node is the top of the stack. Push prepends a node and pop
    unlinks the head — both strictly O(1), no resizing ever required.
    """

    def __init__(self) -> None:
        self._head: Optional[_Node[T]] = None
        self._size = 0

    def push(self, item: T) -> None:
        self._head = _Node(item, self._head)  # O(1), always
        self._size += 1

    def pop(self) -> T:
        if self._head is None:
            raise StackEmptyError("pop from empty stack")
        node = self._head
        self._head = node.next
        self._size -= 1
        return node.value

    def peek(self) -> T:
        if self._head is None:
            raise StackEmptyError("peek at empty stack")
        return self._head.value

    def is_empty(self) -> bool:
        return self._head is None

    def __len__(self) -> int:
        return self._size

    def __repr__(self) -> str:
        items = []
        node = self._head
        while node is not None:
            items.append(node.value)
            node = node.next
        # items[0] is the top; show top -> bottom
        return f"LinkedStack(top -> bottom: {items})"


# ---------------------------------------------------------------------------
# Tradeoffs
# ---------------------------------------------------------------------------
TRADEOFFS = """
Array-backed vs. Linked-list-backed stack
==========================================

Time complexity (both are O(1) for the core ops):
  ArrayStack.push  -> amortized O(1); occasionally O(n) when the backing
                      array doubles in size and all elements are copied.
  ArrayStack.pop   -> O(1) worst case (pop from the end).
  LinkedStack.push -> O(1) worst case, every time (no resizing).
  LinkedStack.pop  -> O(1) worst case.

So the linked list wins on *worst-case latency* per operation, while the
array wins on *amortized* cost. If you need predictable, bounded per-push
time (real-time systems), the linked list is preferable.

Memory:
  ArrayStack  -> compact: one contiguous block, ~1 pointer per slot, but it
                 may hold up to ~2x capacity due to growth slack. Great cache
                 locality (fast to traverse / iterate).
  LinkedStack -> one heap-allocated node per element, each with an extra
                 `next` pointer (and Python object overhead). More total
                 memory, poor cache locality, more GC pressure. But it only
                 ever allocates exactly what it needs — no unused slack.

Other considerations:
  - The array can shrink/grow; growth triggers occasional O(n) copies.
  - The linked list never needs to move existing elements, so references to
    nodes stay valid (useful if other structures point into it).
  - In practice, in Python, ArrayStack (a plain list) is faster and simpler
    for almost all use cases thanks to cache locality and C-level append/pop.

Rule of thumb: default to the array-backed stack. Reach for the linked-list
version when you specifically need guaranteed O(1) worst-case push (no
resize pauses) or stable node identity.
"""


# ---------------------------------------------------------------------------
# Demonstration
# ---------------------------------------------------------------------------
def _demo(stack: ArrayStack[int] | LinkedStack[int]) -> None:
    name = type(stack).__name__
    print(f"--- {name} ---")
    print(f"empty? {stack.is_empty()}")

    for x in (1, 2, 3):
        stack.push(x)
        print(f"push({x}) -> {stack}  (peek={stack.peek()}, len={len(stack)})")

    print(f"pop() -> {stack.pop()}  (now {stack})")
    print(f"pop() -> {stack.pop()}  (now {stack})")
    print(f"empty? {stack.is_empty()}, len={len(stack)}")
    print(f"pop() -> {stack.pop()}  (now {stack})")

    try:
        stack.pop()
    except StackEmptyError as e:
        print(f"pop() on empty raised StackEmptyError: {e}")
    print()


def main() -> None:
    _demo(ArrayStack())
    _demo(LinkedStack())
    print(TRADEOFFS)


if __name__ == "__main__":
    main()
