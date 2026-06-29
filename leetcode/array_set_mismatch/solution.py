def find_error_nums(nums: list[int]) -> tuple[int, int]:
    """
    You have a set of integers s, which originally contains all the numbers from 1 to n. Unfortunately, due to some error, one of the numbers in s got duplicated to another number in the set, which results in repetition of one number and loss of another number.

    You are given an integer array nums representing the data status of this set after the error.

    Find the number that occurs twice and the number that is missing 
    """
    seen_mask: int = 0
    dup: int = 0
    missing: int = 0

    for num in nums:
        # use zero-indexed bits to simplify operations
        num_mask = 1 << (num - 1)
        if seen_mask & num_mask:
            # bitwise and reveals the duplicate
            dup = num
        # mark the number's bit as seen
        seen_mask |= num_mask

    # invert the seen bits to reveal the missing number
    all_ones = (1 << len(nums)) - 1
    inverted = seen_mask ^ all_ones
    # add one back to convert back to the 1-based sequence of nums
    missing = inverted.bit_length() - 1 + 1

    return dup, missing


def find_error_nums_math(nums: list[int]) -> tuple[int, int]:
    """
    Solves the problem using sum and sum of squares equations.
    Time Complexity: O(N)
    Space Complexity: O(1)
    """
    n = len(nums)
    
    # Expected sums
    sum_expected = n * (n + 1) // 2
    sum_sq_expected = n * (n + 1) * (2 * n + 1) // 6
    
    # Actual sums
    sum_actual = sum(nums)
    sum_sq_actual = sum(x * x for x in nums)
    
    # Differences
    diff_sum = sum_expected - sum_actual  # missing - dup
    diff_sq = sum_sq_expected - sum_sq_actual  # missing^2 - dup^2
    
    # (missing^2 - dup^2) / (missing - dup) = missing + dup
    sum_missing_dup = diff_sq // diff_sum
    
    missing = (sum_missing_dup + diff_sum) // 2
    dup = sum_missing_dup - missing
    
    return dup, missing


def find_error_nums_xor(nums: list[int]) -> tuple[int, int]:
    """
    Solves the problem using XOR partitioning.
    Time Complexity: O(N)
    Space Complexity: O(1)
    """
    xor_all = 0
    for i, num in enumerate(nums, 1):
        xor_all ^= num ^ i
        
    rightmost_bit = xor_all & -xor_all
    
    xor_group1 = 0
    xor_group2 = 0
    
    for num in nums:
        if num & rightmost_bit:
            xor_group1 ^= num
        else:
            xor_group2 ^= num
            
    for i in range(1, len(nums) + 1):
        if i & rightmost_bit:
            xor_group1 ^= i
        else:
            xor_group2 ^= i
            
    if xor_group1 in nums:
        return xor_group1, xor_group2
    else:
        return xor_group2, xor_group1


if __name__ == "__main__":
    print("Original: ", find_error_nums([1, 2, 3, 3, 5]))
    print("Math:     ", find_error_nums_math([1, 2, 3, 3, 5]))
    print("XOR:      ", find_error_nums_xor([1, 2, 3, 3, 5]))
