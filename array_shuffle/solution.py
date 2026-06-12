import itertools

"""
def shuffle(, nums: list[int], n: int) -> List[int]:
    x = nums[:n]
    y = nums[n:]
    merged = []
    for i in range(n):
        merged.append(x[i])
        merged.append(y[i])
    return merged
"""

def shuffle(nums: list[int], n: int) -> list[int]:
    x = nums[:n]
    y = nums[n:]
    return list(itertools.chain.from_iterable(zip(x, y)))

if __name__ == "__main__":
    print(shuffle([1, 3, 5, 2, 4, 6], 3))

