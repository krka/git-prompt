#!/usr/bin/env python3
"""
Analyze function pointer references and their impact on binary size.

This script:
1. Finds all function pointer references (&function_name) in Git source
2. Builds a call graph to determine reachability from each pointer
3. Calculates how many functions could be eliminated by removing each pointer
4. Prioritizes patches by total bytes that could be saved

This helps identify high-impact vtable entries and callbacks to patch out.
"""

import re
import subprocess
import sys
from pathlib import Path
from collections import defaultdict, deque
from typing import Dict, Set, List, Tuple

def find_function_pointers(git_dir: Path) -> Dict[str, List[Tuple[str, int]]]:
    """
    Find all function pointer references (&func) in C source files.

    Returns: dict mapping function_name -> [(file, line_number), ...]
    """
    function_refs = defaultdict(list)

    # Search for &function_name patterns in C files
    result = subprocess.run(
        ['grep', '-rn', '-E', r'&[a-zA-Z_][a-zA-Z0-9_]+', '--include=*.c', '--include=*.h', str(git_dir)],
        capture_output=True, text=True, check=False
    )

    for line in result.stdout.split('\n'):
        if not line:
            continue

        # Parse grep output: file:line:content
        parts = line.split(':', 2)
        if len(parts) < 3:
            continue

        file_path, line_num, content = parts

        # Extract function names from &func references
        for match in re.finditer(r'&([a-zA-Z_][a-zA-Z0-9_]+)', content):
            func_name = match.group(1)
            # Filter out likely non-function references
            if not func_name.isupper():  # Skip ALL_CAPS (likely constants)
                rel_path = Path(file_path).relative_to(git_dir)
                function_refs[func_name].append((str(rel_path), int(line_num)))

    return function_refs

def get_all_symbols(binary: Path) -> Dict[str, int]:
    """Extract all text symbols and their sizes from binary."""
    result = subprocess.run(['nm', '-S', '--defined-only', binary],
                          capture_output=True, text=True, check=True)

    symbols = {}
    for line in result.stdout.strip().split('\n'):
        parts = line.split()
        # nm -S format: address [size] type name
        # With size: address size type name (4 parts)
        # Without size: address type name (3 parts)

        if len(parts) >= 4 and parts[2] in ['T', 't']:
            # Format: address size type name
            try:
                size = int(parts[1], 16)
                symbols[parts[3]] = size
            except ValueError:
                # Size parsing failed, skip this symbol
                pass
        elif len(parts) == 3 and parts[1] in ['T', 't']:
            # Format: address type name (no size info)
            symbols[parts[2]] = 1  # Use 1 as default size

    return symbols

def get_calls_from_function(func_name: str, binary: Path, symbols: Set[str]) -> Set[str]:
    """Extract function calls from a specific function using objdump."""
    try:
        result = subprocess.run(
            ['objdump', '-d', f'--disassemble={func_name}', binary],
            capture_output=True, text=True, check=True, timeout=5
        )

        calls = set()
        for line in result.stdout.split('\n'):
            match = re.search(r'callq?\s+[0-9a-f]+\s+<([^@>]+)', line)
            if match:
                called = match.group(1)
                if called in symbols and called != func_name:
                    calls.add(called)
        return calls
    except:
        return set()

def build_call_graph(binary: Path, symbols: Set[str]) -> Dict[str, Set[str]]:
    """Build call graph: function -> set of functions it calls."""
    print("Building call graph (this may take a minute)...", file=sys.stderr)
    call_graph = {}

    for i, func in enumerate(symbols):
        if i % 100 == 0:
            print(f"  Progress: {i}/{len(symbols)} functions analyzed", file=sys.stderr)
        call_graph[func] = get_calls_from_function(func, binary, symbols)

    print(f"  Complete: {len(symbols)} functions analyzed", file=sys.stderr)
    return call_graph

def calculate_reachable_from(func: str, call_graph: Dict[str, Set[str]]) -> Set[str]:
    """
    Calculate all functions reachable from a given root function.
    Uses BFS traversal.
    """
    reachable = set()
    queue = deque([func])

    while queue:
        current = queue.popleft()
        if current in reachable:
            continue
        reachable.add(current)

        for called in call_graph.get(current, set()):
            if called not in reachable:
                queue.append(called)

    return reachable

def analyze_impact(binary: Path, git_dir: Path) -> None:
    """Main analysis function."""
    print("=" * 70)
    print("Function Pointer Impact Analysis")
    print("=" * 70)
    print()

    # Find function pointers in source
    print("1. Finding function pointer references in source...", file=sys.stderr)
    func_refs = find_function_pointers(git_dir)
    print(f"   Found {len(func_refs)} unique function pointers", file=sys.stderr)
    print()

    # Get symbols from binary
    print("2. Extracting symbols from binary...", file=sys.stderr)
    symbols_with_size = get_all_symbols(binary)
    symbols = set(symbols_with_size.keys())
    print(f"   Found {len(symbols)} functions in binary", file=sys.stderr)
    print()

    # Build call graph
    print("3. Building call graph...", file=sys.stderr)
    call_graph = build_call_graph(binary, symbols)
    print()

    # Calculate reachability from main (to identify what's already reachable)
    print("4. Calculating reachability from main...", file=sys.stderr)
    reachable_from_main = calculate_reachable_from('main', call_graph)
    print(f"   {len(reachable_from_main)} functions reachable from main", file=sys.stderr)
    print()

    # Analyze impact of each function pointer
    print("5. Analyzing impact of function pointers...", file=sys.stderr)
    impacts = []

    for func_name in func_refs:
        # Only analyze if function exists in binary and is NOT reachable from main
        if func_name in symbols and func_name not in reachable_from_main:
            reachable = calculate_reachable_from(func_name, call_graph)
            # Calculate total size
            total_size = sum(symbols_with_size.get(f, 0) for f in reachable)
            impacts.append({
                'function': func_name,
                'reachable_count': len(reachable),
                'total_bytes': total_size,
                'references': func_refs[func_name]
            })

    # Sort by total bytes (highest impact first)
    impacts.sort(key=lambda x: x['total_bytes'], reverse=True)

    print()
    print("=" * 70)
    print("TOP FUNCTION POINTERS BY ELIMINATION POTENTIAL")
    print("=" * 70)
    print()

    # Check if we have size info
    has_sizes = any(symbols_with_size.get(f, 0) > 1 for f in symbols_with_size)

    if has_sizes:
        print(f"{'Function':<40} {'Reach':<8} {'Bytes':<10} {'References'}")
    else:
        print(f"{'Function':<40} {'Reach':<8} {'References':<12} {'Location'}")
    print("-" * 70)

    for impact in impacts[:30]:  # Top 30
        func = impact['function']
        reach = impact['reachable_count']
        size = impact['total_bytes']
        refs = len(impact['references'])

        # Show first reference location
        first_ref = impact['references'][0]
        location = f"{first_ref[0]}:{first_ref[1]}"

        if has_sizes:
            print(f"{func:<40} {reach:<8} {size:<10} {refs:>3}x  {location}")
        else:
            print(f"{func:<40} {reach:<8} {refs:<12} {location}")

    print()
    print("Legend:")
    print("  Function = Function pointer name")
    print("  Reach    = Number of functions reachable from this pointer")
    if has_sizes:
        print("  Bytes    = Total bytes that could be eliminated")
    print("  References = Number of references (&func) in source")
    print()

    # Show detailed analysis for top 5
    print("=" * 70)
    print("DETAILED ANALYSIS - TOP 5 CANDIDATES")
    print("=" * 70)
    print()

    for i, impact in enumerate(impacts[:5]):
        print(f"{i+1}. {impact['function']}")
        print(f"   Potential savings: {impact['total_bytes']} bytes ({impact['reachable_count']} functions)")
        print(f"   References ({len(impact['references'])}):")
        for ref_file, ref_line in impact['references'][:3]:  # Show first 3
            print(f"     • {ref_file}:{ref_line}")
        if len(impact['references']) > 3:
            print(f"     ... and {len(impact['references']) - 3} more")
        print()

def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        print("Usage: analyze-vtable-impact.py <binary> [git-source-dir]", file=sys.stderr)
        print("\nAnalyzes function pointers and their elimination potential", file=sys.stderr)
        sys.exit(1)

    binary = Path(sys.argv[1])
    git_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('submodules/git')

    if not binary.exists():
        print(f"Error: Binary not found: {binary}", file=sys.stderr)
        sys.exit(1)

    if not git_dir.exists():
        print(f"Error: Git source directory not found: {git_dir}", file=sys.stderr)
        sys.exit(1)

    analyze_impact(binary, git_dir)

if __name__ == '__main__':
    main()
