# Array Shuffle

A highly optimized, Pythonic implementation for interleaving (shuffling) two halves of an array. Given an array of $2n$ elements in the form `[x1, x2, ..., xn, y1, y2, ..., yn]`, it returns the array in the form `[x1, y1, x2, y2, ..., xn, yn]`.

For example, input lists like `[1, 3, 5]` and `[2, 4, 6]` are interleaved to become `[1, 2, 3, 4, 5, 6]`.

## Implementation

The solution uses Python's extended slicing syntax:

```python
def shuffle(nums: list[int], n: int) -> list[int]:
    res = [0] * len(nums)
    res[::2] = nums[:n]
    res[1::2] = nums[n:]
    return res
```

---

## Why Slicing is the Best Approach

### 1. Performance (C-Level Execution)
In Python, operations executed inside a Python-level loop (like a standard `for` loop or generator expressions inside `itertools`) incur interpreter overhead for each iteration. In contrast, slice assignments like `res[::2] = ...` are implemented directly in C under the hood. The data is copied in contiguous blocks in memory, which is significantly faster.

### 2. Pre-allocation and Memory Efficiency
When you append to a list in a loop, Python periodically has to dynamically resize the list's memory bucket as it grows. With slice assignment, we pre-allocate the exact list size needed using `res = [0] * len(nums)`. This allocates the memory in a single step, preventing any dynamic resizing overhead.

### 3. Readability and Expressiveness
Using extended slicing notation clearly expresses the intent of interleaving:
- `res[::2]` targets every even index (e.g., `0, 2, 4...`) where the first half of the array belongs.
- `res[1::2]` targets every odd index (e.g., `1, 3, 5...`) where the second half of the array belongs.

It maps 1-to-1 with the conceptual operation of interleaving.

### 4. Zero Dependencies
Unlike an `itertools.chain.from_iterable(zip(...))` approach, it does not require importing any standard library modules, keeping the code clean and self-contained.

---

## Slicing Syntax Explained

Python's slicing syntax follows the format `sequence[start:stop:step]`.

### 1. Extracting the Halves (`nums[:n]` and `nums[n:]`)
*   **`nums[:n]`**: Retrieves elements starting from index `0` up to (but not including) index `n` (the first half of the list).
*   **`nums[n:]`**: Retrieves elements starting from index `n` up to the very end of the list (the second half of the list).

### 2. Targeting Even Indices (`res[::2] = ...`)
*   **`start` (omitted)**: Starts at `0`.
*   **`stop` (omitted)**: Goes to the end of the list.
*   **`step` (`2`)**: Grabs every second element.
*   **Target Indices**: `0, 2, 4, 6, 8, ...`

This assigns the elements of the first half of the array to all the even-positioned slots.

### 3. Targeting Odd Indices (`res[1::2] = ...`)
*   **`start` (`1`)**: Starts at index `1`.
*   **`stop` (omitted)**: Goes to the end of the list.
*   **`step` (`2`)**: Grabs every second element.
*   **Target Indices**: `1, 3, 5, 7, 9, ...`

This assigns the elements of the second half of the array to all the odd-positioned slots, successfully interleaving the two halves.

---

## Running the Project

This project uses [Pixi](https://pixi.sh) for environment and task management.

### Run the Main Script
```bash
pixi run main
```

### Run Unit Tests
```bash
pixi run test
```
