
def smaller_numbers_than_current(nums: list[int]) -> list[int]:
    """
    Given the array nums, for each nums[i] find out how many numbers in the array are smaller than it. That is, for each nums[i] you have to count the number of valid j's such that j != i and nums[j] < nums[i].

    Return the answer in an array.
    """
    n = len(nums)
    counts_smaller = [0] * n

    for i in range(n):
        for j in range(n):
            if nums[j] < nums[i]:
                counts_smaller[i] += 1

    return counts_smaller

if __name__ == "__main__":
    print("how many numbers are smaller than the current number?")
    nums = [8, 1, 2, 2, 3]
    print(f"nums: {nums}")
    print(f"counts: {smaller_numbers_than_current(nums)}")
