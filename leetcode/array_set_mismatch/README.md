# Array Set Mismatch Solutions

This repository provides three different implementations in Python to solve the "Set Mismatch" problem.

## Problem Description
You have a set of integers $s$, which originally contains all the numbers from $1$ to $n$. Unfortunately, due to some error, one of the numbers in $s$ got duplicated to another number in the set, which results in a repetition of one number and loss of another number.

Given an integer array `nums` representing the data status of this set after the error, find the number that occurs twice (duplicate) and the number that is missing.

---

## Implementations

### 1. Original Solution (Bitmask)
Uses a single Python arbitrary-precision integer as a bitmask to keep track of seen numbers.

* **Code**: `find_error_nums(nums)` in [solution.py](file:///home/bob/code/scratchpad/array_set_mismatch/solution.py)
* **Time Complexity**: $O(N)$
* **Space Complexity**: $O(N)$ (requires storing an $N$-bit integer mask)

---

### 2. Math Solution (Sum & Sum-of-Squares)
Uses algebraic equations derived from the actual vs. expected sum and sum of squares of the array elements.

* **Code**: `find_error_nums_math(nums)` in [solution.py](file:///home/bob/code/scratchpad/array_set_mismatch/solution.py)
* **Time Complexity**: $O(N)$
* **Space Complexity**: $O(1)$ auxiliary space
* **How it works**:
  * $S_{\text{expected}} - S_{\text{actual}} = \text{missing} - \text{dup}$
  * $S^2_{\text{expected}} - S^2_{\text{actual}} = \text{missing}^2 - \text{dup}^2 = (\text{missing} - \text{dup})(\text{missing} + \text{dup})$
  * Division yields $\text{missing} + \text{dup}$, from which both values are easily resolved.

---

### 3. XOR Partitioning Solution
Uses bitwise XOR to separate the duplicate and missing numbers without creating large integers.

* **Code**: `find_error_nums_xor(nums)` in [solution.py](file:///home/bob/code/scratchpad/array_set_mismatch/solution.py)
* **Time Complexity**: $O(N)$
* **Space Complexity**: $O(1)$ auxiliary space
* **How it works**:
  * XORing all elements and numbers $1$ to $n$ yields $\text{dup} \oplus \text{missing}$.
  * Finding the rightmost set bit allows partitioning all numbers into two groups.
  * XORing each group isolates the two values.

---

## Complexity Comparison

| Approach | Time Complexity | Space Complexity | Pros | Cons |
| :--- | :--- | :--- | :--- | :--- |
| **Bitmask (Original)** | $O(N)$ | $O(N)$ | Simple bitwise tracking | Allocates memory for large integers |
| **Math (Sums)** | $O(N)$ | $O(1)$ | Fast, uses $O(1)$ memory | Requires sum-of-squares calculation |
| **XOR Partitioning** | $O(N)$ | $O(1)$ | No large integer overhead | Harder to read / more logic |

---

## Running Tests
Run the unit tests using `pixi`:

```bash
pixi run test
```
