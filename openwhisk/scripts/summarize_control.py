#!/usr/bin/env python3
import csv
import sys

import control_benchmark


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT.csv OUTPUT.csv", file=sys.stderr)
        return 2
    with open(sys.argv[1], newline="") as handle:
        rows = list(csv.DictReader(handle))
    control_benchmark.write_summary(rows, sys.argv[2])
    print(f"control_summary samples={len(rows)} output={sys.argv[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
