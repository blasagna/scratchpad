def find_max_consecutive_ones(nums: list[int]) -> int:
    """given an array nums containing only values 0 and 1, return the maximum number of consecutive 1 values in the array"""
    max_count: int = 0
    curr_count: int = 0
    for num in nums:
        if num:
            curr_count += 1
        else:
            if curr_count > max_count:
                max_count = curr_count
            curr_count = 0

    return curr_count if curr_count > max_count else max_count


if __name__ == "__main__":
    print(find_max_consecutive_ones([1, 1, 0, 1, 1, 1]))
    print(find_max_consecutive_ones([1, 0, 1, 1, 0, 1]))
