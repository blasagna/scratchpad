def exclusive_time(n: int, logs: list[str]) -> list[int]:
    result = [0] * n
    stack: list[int] = []  # stack of running function IDs
    prev = 0  # timestamp from which the current top has been running

    for log in logs:
        fn_str, event, ts_str = log.split(":")
        fn_id, ts = int(fn_str), int(ts_str)

        if event == "start":
            if stack:
                # pause the currently running function up to just before ts
                result[stack[-1]] += ts - prev
            stack.append(fn_id)
            prev = ts
        else:  # "end"
            # this function ran through the end of ts, so include ts itself
            result[stack.pop()] += ts - prev + 1
            # the resumed function restarts at the next timestamp
            prev = ts + 1

    return result


if __name__ == "__main__":
    n = 2
    logs = ["0:start:0", "1:start:2", "1:end:5", "0:end:6"]
    print(f"n = {n}, logs = {logs}")
    print(f"exclusive time = {exclusive_time(n, logs)}")  # [3, 4]
