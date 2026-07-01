def find_missing_nums(nums: list[int]) -> list[int]:
    """
    Find all the numbers that are missing from the array.

    Args:
    nums (list[int]): The input array containing numbers.

    Returns:
    list[int]: A list of missing numbers.
    """
    n = len(nums)
    # Create a set of all numbers from 1 to n
    full_set = set(range(1, n + 1))
    # Create a set from the input array
    nums_set = set(nums)
    # The missing numbers are those in full_set but not in nums_set
    missing_nums = list(full_set - nums_set)

    return missing_nums


if __name__ == "__main__":
    # Example usage
    input_nums = [4, 3, 2, 7, 8, 2, 3, 1]
    missing = find_missing_nums(input_nums)
    print(f"Missing numbers: {missing}")

    input_nums = [1, 1]
    missing = find_missing_nums(input_nums)
    print(f"Missing numbers: {missing}")
