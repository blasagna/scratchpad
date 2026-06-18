
def smaller_numbers_than_current_sorted(nums: list[int]) -> list[int]:
    """
    O(N log N) time complexity using sorting and a dictionary lookup.
    """
    sorted_nums = sorted(nums)
    counts = {}
    for i, num in enumerate(sorted_nums):
        if num not in counts:
            counts[num] = i
    return [counts[num] for num in nums]


def smaller_numbers_than_current_counting(nums: list[int]) -> list[int]:
    """
    O(N + K) time complexity using counting sort / prefix sums,
    where K is the range of values (max_val - min_val).
    """
    if not nums:
        return []

    min_val = min(nums)
    max_val = max(nums)
    k = max_val - min_val

    # Shifting values by -min_val to support negative numbers
    count = [0] * (k + 2)
    for num in nums:
        count[num - min_val + 1] += 1

    for i in range(1, len(count)):
        count[i] += count[i - 1]

    return [count[num - min_val] for num in nums]


def smaller_numbers_than_current(nums: list[int]) -> list[int]:
    """
    Given the array nums, for each nums[i] find out how many numbers in the array are smaller than it.
    """
    return smaller_numbers_than_current_sorted(nums)

if __name__ == "__main__":
    print("how many numbers are smaller than the current number?")
    nums = [8, 1, 2, 2, 3]
    print(f"nums: {nums}")
    print(f"counts: {smaller_numbers_than_current(nums)}")
