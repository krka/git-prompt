# Analysis Tools for git-prompt

This directory contains scripts and documentation for analyzing the git-prompt binary.

## Scripts

### apply-patches.py
Python-based patching system that applies source modifications to Git before building.

**Usage:**
```bash
./tools/apply-patches.py [submodule-dir]
```

**Default directory:** `submodules/git`

**How it works:**
- Reads patch definitions from `patches.yaml`
- Applies each patch with automatic whitespace handling
- Fails explicitly if patterns don't match (detects Git version changes)
- Used automatically by the Makefile during builds

**Patch Configuration:** Edit `tools/patches.yaml` to add/modify patches

**See also:** `PATCHING.md` for detailed documentation on creating patches

### analyze-binary.sh
Analyzes a binary to identify reachable and unreachable functions.

**Usage:**
```bash
./tools/analyze-binary.sh [binary-path]
```

**Default binary:** `git-prompt-optimized`

**Output:**
- Binary size and section information
- Total function count
- Reachable functions (via BFS traversal from main())
- Unreachable functions (dead code)
- Top 20 largest unreachable functions
- Analysis files saved to `/tmp/`:
  - `all_funcs.txt` - All functions in binary
  - `reachable_funcs.txt` - Functions reachable from main()
  - `unreachable_funcs.txt` - Dead code functions

**Example:**
```bash
$ ./tools/analyze-binary.sh git-prompt-optimized
=== Binary Analysis: git-prompt-optimized ===

Binary size: 928K

Total functions in binary: 298
Reachable functions from main: 33
Unreachable functions: 265
Unreachable percentage: 88%
```

## Documentation

### UNREACHABLE_CODE_ANALYSIS.md
Comprehensive analysis explaining why LTO cannot eliminate certain unreachable functions, including:
- Virtual function tables (vtables) for ref storage backends
- Hash algorithm polymorphism  
- Trace2 instrumentation functions
- Comparison function pointers
- Iterator vtables

Key finding: ~265 functions (415KB) remain unreachable due to C vtable polymorphism. This represents the fundamental limit of LTO optimization without source code modifications.

## Requirements

- `nm` - For symbol extraction
- `objdump` - For disassembly and call graph analysis
- `python3` - For BFS traversal
- `size` - For section size analysis

## Notes

- The optimized build (`git-prompt-optimized`) must have debug symbols for analysis
- The stripped build (`git-prompt-stripped`) cannot be analyzed (no symbols)
- Analysis uses BFS (breadth-first search) to traverse call graph from main()
- Function reachability is determined by disassembling and finding `callq` instructions
