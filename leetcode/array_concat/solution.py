def get_concatenation(nums: List[int]) -> List[int]:
    return nums + nums


if __name__ == "__main__":
    nums = [1, 2, 3]
    nums_cat = get_concatenation(nums)
    print(nums_cat)
