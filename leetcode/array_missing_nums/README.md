# Find all numbers disappeared in an array

Given an array nums of n integers where nums[i] is in the range [1, n], return an array of all the integers in the range [1, n] that do not appear in nums.

 
## Examples

Example 1:
```
Input: nums = [4,3,2,7,8,2,3,1]
Output: [5,6]
```
Example 2:
```
Input: nums = [1,1]
Output: [2]
 ```

## Constraints:

1. n == nums.length
1. 1 <= n <= 105
1. 1 <= nums[i] <= n

## Complexity analysis

Let **n** be the length of `nums`.

### Current solution (set-based)

The implementation in `solution.py` builds a set of `1..n`, a set of the input,
and takes their difference:

```python
full_set = set(range(1, n + 1))    # O(n) time, O(n) space
nums_set = set(nums)               # O(n) time, O(n) space
missing_nums = list(full_set - nums_set)  # O(n) time, O(n) space
```

- **Time: O(n)** — each set is built in O(n), and the set difference iterates
  over `full_set` (n elements) doing O(1) average hash lookups in `nums_set`.
- **Space: O(n)** — two sets plus the output list, each holding up to n
  elements. This is auxiliary space *excluding* the returned list.

It's readable and linear, but the two sets cost O(n) extra memory.

### Alternative (O(1) extra space, in-place marking)

This is the classic trick for this problem: since every value is in the range
`[1, n]`, the array can encode "have I seen value `v`?" in its own sign bits.
For each value `v`, mark index `v - 1` as seen by negating the number stored
there. Afterwards, any index that is still positive was never marked, so
`index + 1` is a missing number.

```python
def find_missing_nums(nums: list[int]) -> list[int]:
    for v in nums:
        i = abs(v) - 1          # index this value maps to
        if nums[i] > 0:
            nums[i] = -nums[i]  # mark index i as "seen"

    return [i + 1 for i in range(len(nums)) if nums[i] > 0]
```

- **Time: O(n)** — one pass to mark, one pass to collect.
- **Space: O(1)** — no extra data structures; the output list doesn't count as
  auxiliary space.

Trade-off: it mutates the input array (the signs change). If the caller needs
`nums` preserved, restore it with a final pass (`nums[i] = abs(nums[i])`) or
use the set-based version instead.

#### Worked example

`nums = [4, 3, 2, 7, 8, 2, 3, 1]` (n = 8, using 1-based values, 0-based indices)

Marking pass — for each value `v`, negate `nums[abs(v) - 1]`:

| value `v` | index `abs(v)-1` | array after marking that index                  |
|-----------|------------------|-------------------------------------------------|
| 4         | 3                | `[4, 3, 2, -7, 8, 2, 3, 1]`                      |
| 3         | 2                | `[4, 3, -2, -7, 8, 2, 3, 1]`                     |
| 2         | 1                | `[4, -3, -2, -7, 8, 2, 3, 1]`                    |
| 7         | 6                | `[4, -3, -2, -7, 8, 2, -3, 1]`                   |
| 8         | 7                | `[4, -3, -2, -7, 8, 2, -3, -1]`                  |
| 2         | 1                | already negative → unchanged                    |
| 3         | 2                | already negative → unchanged                    |
| 1         | 0                | `[-4, -3, -2, -7, 8, 2, -3, -1]`                 |

Collecting pass — indices still positive are `4` and `5`, so the missing
numbers are `4 + 1 = 5` and `5 + 1 = 6` → **`[5, 6]`**.