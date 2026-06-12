def shuffle(nums: list[int], n: int) -> list[int]:
    res = [0] * len(nums)
    res[::2] = nums[:n]
    res[1::2] = nums[n:]
    return res

if __name__ == "__main__":
    print(shuffle([1, 3, 5, 2, 4, 6], 3))
