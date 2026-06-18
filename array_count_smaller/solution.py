
def smaller_numbers_than_current(nums: list[int]) -> list[int]:
    """
    Given the array nums, for each nums[i] find out how many numbers in the array are smaller than it. That is, for each nums[i] you have to count the number of valid j's such that j != i and nums[j] < nums[i].

    Return the answer in an array.
    """
    if not nums:
        return []

    sorted_nums = sorted(nums)
    counts = {}
    counts[sorted_nums[0]] = 0

    for i in range(1, len(sorted_nums)):
        if sorted_nums[i] == sorted_nums[i - 1]:
            # Shares the same count of smaller numbers as its left adjacent neighbor
            pass
        else:
            # The number of smaller elements is equal to the index i
            counts[sorted_nums[i]] = i

    return [counts[num] for num in nums]

if __name__ == "__main__":
    print("how many numbers are smaller than the current number?")
    nums = [8, 1, 2, 2, 3]
    print(f"nums: {nums}")
    print(f"counts: {smaller_numbers_than_current(nums)}")
