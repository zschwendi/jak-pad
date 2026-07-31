#!/usr/bin/env python3
"""Summarize a goalc-cbackend-sweep report.

Prints aggregate coverage, the worst files by unemitted function count, and a histogram of
the construct that blocked each function that could not be emitted. The backend stops at the
first unsupported construct in a function, so the histogram counts first blockers.
"""

import argparse
import collections
import re
import sys

IR_NODE = re.compile(r"^no C lowering for IR node: +(\S+)")
PS2_OP = re.compile(r"^no C lowering for this PS2 128-bit integer operation: +(\S+)")


def classify(detail):
    m = IR_NODE.match(detail)
    if m:
        return "IR node " + m.group(1)
    m = PS2_OP.match(detail)
    if m:
        return "PS2 128-bit int op " + m.group(1)
    if detail.startswith("behavior functions pass self"):
        return "behavior `self` in a fixed machine register (r13)"
    m = re.match(r"^rlet :reset-here on machine register (\S+?),", detail)
    if m:
        return "rlet :reset-here on machine register " + m.group(1)
    return detail


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report")
    parser.add_argument("--worst", type=int, default=25)
    args = parser.parse_args()

    files = []
    causes = collections.Counter()
    cause_files = collections.defaultdict(set)
    file_errors = []
    with open(args.report) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if parts[0] == "FILE":
                files.append((parts[1], int(parts[2]), int(parts[3])))
            elif parts[0] == "FAIL":
                cause = classify(parts[3])
                causes[cause] += 1
                cause_files[cause].add(parts[1])
            elif parts[0] == "FILEERROR":
                file_errors.append((parts[1], parts[3]))

    emitted = sum(f[1] for f in files)
    total = sum(f[2] for f in files)
    print(f"files compiled: {len(files)}  front-end failures: {len(file_errors)}")
    print(f"functions: {emitted}/{total} emitted ({100.0 * emitted / max(total, 1):.2f}%)")
    full = [f for f in files if f[1] == f[2]]
    print(f"files at 100%: {len(full)}/{len(files)}")
    print()

    print(f"top {args.worst} files by unemitted functions:")
    for path, ok, tot in sorted(files, key=lambda f: f[1] - f[2])[: args.worst]:
        print(f"  {ok:5d}/{tot:5d}  {100.0 * ok / max(tot, 1):6.1f}%  {path}")
    print()

    print("blocking causes (first blocker per function), ranked:")
    for cause, count in causes.most_common():
        print(f"  {count:6d} functions  {len(cause_files[cause]):4d} files  {cause}")

    for path, err in file_errors:
        print(f"FRONT-END FAILURE {path}: {err}", file=sys.stderr)


if __name__ == "__main__":
    main()
