# Array Count Smaller

This project provides different implementations of the `smaller_numbers_than_current` problem: given an array `nums`, for each `nums[i]` find out how many numbers in the array are smaller than it.

## Approaches

### 1. Sorting & Enumerate Lookup (`smaller_numbers_than_current_sorted`)
This approach first sorts the input array. Because it is sorted, the first occurrence of any number at index `i` is exactly the count of elements smaller than it. We construct a lookup dictionary by enumerating the sorted list and storing the index of the first occurrence of each value. Finally, we map each element in the original list to its smaller count from the dictionary.

- **Time Complexity:** $O(N \log N)$ to sort the array of size $N$. Dictionary construction and output generation take $O(N)$ time.
- **Space Complexity:** $O(N)$ to store the sorted array and the dictionary.

### 2. Counting Sort / Prefix Sum (`smaller_numbers_than_current_counting`)
This approach uses counting sort. We count frequencies of each number and compute prefix sums to find the total count of elements smaller than each value. To handle negative numbers, values are shifted relative to the minimum element.

- **Time Complexity:** $O(N + K)$ where $N$ is the number of elements and $K$ is the range of values (`max_val - min_val`).
- **Space Complexity:** $O(K)$ to store the frequency array where $K$ is the range of values.

---

## Complexity Comparison

| Approach | Time Complexity | Space Complexity | Best Use Case |
|---|---|---|---|
| **Sorting & Enumerate** | $O(N \log N)$ | $O(N)$ | General use; large range of values $K$. |
| **Counting Sort / Prefix Sum** | $O(N + K)$ | $O(K)$ | Bounded/small range of values $K$. |

---

## Usage & Tests

To run the unit tests, use the following `pixi` command:

```bash
pixi run test
```
