#!/bin/sh
# Usage: run_probe.sh <docker exec prefix...>   e.g. run_probe.sh docker exec -i -u postgres ju_scratch psql -X -A -F ' | ' postgres
# Prints every case where expected != actual, then a total.
"$@" < "$(dirname "$0")/bug_probe.sql" | grep -E '^[A-H][0-9]' | python3 -c '
import sys
n = ok = 0
for line in sys.stdin:
    cid, _desc, exp, act = line.rstrip("\n").split(" | ")[:4]
    n += 1
    if exp == act: ok += 1
    else: print("DIFF", cid, "| expected:", exp, "| actual:", act)
print(f"{ok}/{n} cases match")
'
