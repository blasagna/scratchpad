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

if __name__ == "__main__":
    print(find_error_nums([1, 2, 3, 3, 5]))
