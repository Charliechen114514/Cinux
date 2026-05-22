#!/usr/bin/env python3
"""check_line_limits.py — enforce per-file line-count limits.

Usage:
    python3 scripts/check_line_limits.py [--cpp N] [--hpp N] [path ...]

Defaults:
    --cpp 500   (kernel .cpp / .S files)
    --hpp 300   (kernel .h / .hpp files)

Scans kernel/ (excluding kernel/mini/ and kernel/test/) and exits
non-zero if any file exceeds its limit.
"""

import argparse
import os
import subprocess
import sys


def count_lines(path: str) -> int:
    result = subprocess.run(["wc", "-l", path], capture_output=True, text=True)
    return int(result.stdout.split()[0])


def main() -> int:
    parser = argparse.ArgumentParser(description="Check per-file line-count limits")
    parser.add_argument("--cpp", type=int, default=500, help="Max lines for .cpp/.S files")
    parser.add_argument("--hpp", type=int, default=300, help="Max lines for .h/.hpp files")
    parser.add_argument("paths", nargs="*", default=["kernel"])
    args = parser.parse_args()

    violations = []

    for base in args.paths:
        for root, _dirs, files in os.walk(base):
            # Skip mini kernel and in-kernel test directories
            rel = os.path.relpath(root, ".")
            parts = rel.split(os.sep)
            if "mini" in parts:
                continue
            if "test" in parts:
                continue

            for fname in files:
                fpath = os.path.join(root, fname)
                ext = os.path.splitext(fname)[1].lower()

                if ext in (".cpp", ".s"):
                    limit = args.cpp
                elif ext in (".h", ".hpp"):
                    limit = args.hpp
                else:
                    continue

                n = count_lines(fpath)
                if n > limit:
                    violations.append((fpath, n, limit))

    if violations:
        print("ERROR: files exceed line-count limits:", file=sys.stderr)
        for path, n, limit in violations:
            print(f"  {path}: {n} lines (limit {limit})", file=sys.stderr)
        return 1

    print("OK: all files within line-count limits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
