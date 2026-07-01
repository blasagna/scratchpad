"""Queue implemented with two stacks.

A queue is a FIFO (first-in, first-out) collection supporting:
    enqueue(x) - add x to the back
    dequeue()  - remove and return the front item
    peek()     - return the front item without removing it
    is_empty() / len()

The classic trick is to use two LIFO stacks to get FIFO behavior:

    - `_in`  receives every enqueued item (newest ends up on top).
    - `_out` serves items to the front. When it is empty, we pour the
      whole `_in` stack into it, which *reverses* the order, so the
      oldest item ends up on top of `_out` — exactly what FIFO needs.

We only transfer when `_out` is empty, which is what makes dequeue O(1)
*amortized*: each element is moved from `_in` to `_out` at most once over
its lifetime, even though a single dequeue that triggers a transfer is O(n).
"""

from __future__ import annotations

from typing import Generic, TypeVar

from stack import ArrayStack, StackEmptyError

T = TypeVar("T")


class QueueEmptyError(Exception):
    """Raised when dequeue/peek is called on an empty queue."""


class TwoStackQueue(Generic[T]):
    """FIFO queue backed by two ArrayStacks.

    `_in` collects new items; `_out` dispenses them in FIFO order. Items are
    lazily transferred from `_in` to `_out` only when `_out` runs dry.
    """

    def __init__(self) -> None:
        self._in: ArrayStack[T] = ArrayStack()
        self._out: ArrayStack[T] = ArrayStack()

    def enqueue(self, item: T) -> None:
        """Add an item to the back of the queue. O(1) amortized."""
        self._in.push(item)

    def _transfer(self) -> None:
        """Pour `_in` into `_out`, reversing order, if `_out` is empty."""
        if self._out.is_empty():
            while not self._in.is_empty():
                self._out.push(self._in.pop())

    def dequeue(self) -> T:
        """Remove and return the front item. O(1) amortized, O(n) worst case."""
        self._transfer()
        try:
            return self._out.pop()
        except StackEmptyError:
            raise QueueEmptyError("dequeue from empty queue") from None

    def peek(self) -> T:
        """Return the front item without removing it."""
        self._transfer()
        try:
            return self._out.peek()
        except StackEmptyError:
            raise QueueEmptyError("peek at empty queue") from None

    def is_empty(self) -> bool:
        return self._in.is_empty() and self._out.is_empty()

    def __len__(self) -> int:
        return len(self._in) + len(self._out)

    def __repr__(self) -> str:
        # Front is the top of _out, then the bottom-to-top of _in.
        # Reconstruct front -> back order for display.
        front_to_back = list(reversed(self._out._items)) + list(self._in._items)
        return f"TwoStackQueue(front -> back: {front_to_back})"


# ---------------------------------------------------------------------------
# Demonstration
# ---------------------------------------------------------------------------
def main() -> None:
    q: TwoStackQueue[int] = TwoStackQueue()
    print(f"empty? {q.is_empty()}")

    for x in (1, 2, 3):
        q.enqueue(x)
        print(f"enqueue({x}) -> {q}  (len={len(q)})")

    # First dequeue triggers the _in -> _out transfer.
    print(f"dequeue() -> {q.dequeue()}  (now {q})")
    print(f"peek()    -> {q.peek()}  (now {q})")

    # Interleave: enqueue lands in _in while _out still serves the front.
    q.enqueue(4)
    print(f"enqueue(4) -> {q}  (len={len(q)})")

    while not q.is_empty():
        print(f"dequeue() -> {q.dequeue()}  (now {q})")

    print(f"empty? {q.is_empty()}, len={len(q)}")

    try:
        q.dequeue()
    except QueueEmptyError as e:
        print(f"dequeue() on empty raised QueueEmptyError: {e}")


if __name__ == "__main__":
    main()
