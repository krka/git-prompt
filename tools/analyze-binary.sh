#!/bin/bash
# Analyze binary size and function reachability
# Usage: ./tools/analyze-binary.sh <binary-path>

set -e

BINARY="${1:-git-prompt-optimized}"

if [ ! -f "$BINARY" ]; then
    echo "Error: Binary '$BINARY' not found"
    echo "Usage: $0 <binary-path>"
    exit 1
fi

echo "=== Binary Analysis: $BINARY ==="
echo ""

# Check if binary has symbols
if ! nm "$BINARY" >/dev/null 2>&1; then
    echo "Warning: Binary has no symbols (was stripped with -s flag)"
    echo "Cannot perform function analysis on stripped binaries."
    exit 0
fi

echo "Binary size: $(du -h "$BINARY" | cut -f1)"
echo ""

# Section sizes
echo "=== Section Sizes ==="
size "$BINARY"
echo ""

# Get all defined text symbols
nm --defined-only -g "$BINARY" | awk '$2 == "T" || $2 == "t" {print $3}' | sort > /tmp/all_funcs.txt
TOTAL_FUNCS=$(wc -l < /tmp/all_funcs.txt)

echo "Total functions in binary: $TOTAL_FUNCS"

# BFS traversal from main to find reachable functions
python3 - "$BINARY" << 'PYTHON'
import subprocess
import re
import sys

binary = sys.argv[1]

def get_all_symbols():
    """Extract all text symbols from binary"""
    result = subprocess.run(['nm', '-g', '--defined-only', binary],
                          capture_output=True, text=True, check=True)
    symbols = {}
    for line in result.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ['T', 't']:
            symbols[parts[2]] = parts[0]
    return symbols

def get_calls_from_function(func_name, symbols):
    """Extract function calls from a specific function"""
    if func_name not in symbols:
        return []
    try:
        result = subprocess.run(['objdump', '-d', '--disassemble=' + func_name, binary],
                              capture_output=True, text=True, check=True, timeout=5)
        calls = set()
        for line in result.stdout.split('\n'):
            match = re.search(r'callq?\s+[0-9a-f]+\s+<([^@>]+)', line)
            if match:
                called = match.group(1)
                if called in symbols and called != func_name:
                    calls.add(called)
        return sorted(calls)
    except:
        return []

# BFS from main to find all reachable functions
symbols = get_all_symbols()
visited = set()
queue = ['main']

while queue:
    func = queue.pop(0)
    if func in visited:
        continue
    visited.add(func)

    calls = get_calls_from_function(func, symbols)
    for called in calls:
        if called not in visited:
            queue.append(called)

# Write reachable functions
with open('/tmp/reachable_funcs.txt', 'w') as f:
    for func in sorted(visited):
        f.write(func + '\n')

print(f"Reachable functions from main: {len(visited)}")
PYTHON

REACHABLE=$(wc -l < /tmp/reachable_funcs.txt)
echo "Reachable functions from main: $REACHABLE"

# Find unreachable functions
comm -13 /tmp/reachable_funcs.txt /tmp/all_funcs.txt > /tmp/unreachable_funcs.txt
UNREACHABLE=$(wc -l < /tmp/unreachable_funcs.txt)
echo "Unreachable functions: $UNREACHABLE"

# Calculate percentage
if [ $TOTAL_FUNCS -gt 0 ]; then
    UNREACHABLE_PCT=$((100 * UNREACHABLE / TOTAL_FUNCS))
    echo "Unreachable percentage: ${UNREACHABLE_PCT}%"
fi

echo ""
echo "=== Top 20 Unreachable Functions by Size ==="
while read func; do
    size=$(nm -S "$BINARY" | grep " $func\$" | awk '{print "0x" $2}')
    if [ -n "$size" ]; then
        size_dec=$(printf "%d" $size 2>/dev/null || echo 0)
        echo "$size_dec $func"
    fi
done < /tmp/unreachable_funcs.txt | sort -rn | head -20 | awk '{printf "%6d KB  %s\n", $1/1024, $2}'

echo ""
echo "Full analysis saved to:"
echo "  /tmp/all_funcs.txt - All functions"
echo "  /tmp/reachable_funcs.txt - Reachable from main()"
echo "  /tmp/unreachable_funcs.txt - Dead code"
