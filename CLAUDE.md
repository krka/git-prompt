# git-prompt Project Guide

This document helps Claude Code quickly understand the git-prompt project structure and conventions.

## Project Overview

`git-prompt` is a fast, standalone C program that displays colorful git repository status information optimized for shell prompt integration. It's built on top of libgit.a from the git source tree.

## Project Structure

```
git-prompt/
├── git-prompt.c           # Main source file (~1150 lines)
├── submodules/git/        # Git source tree (submodule)
├── tests/
│   ├── run_tests.py       # Python test runner
│   ├── test_cases.yaml    # Declarative test cases
│   ├── test-results.txt   # Generated text report
│   ├── test-results.html  # Generated detailed HTML report
│   └── examples.html      # Generated summary HTML report
├── tools/                 # Helper scripts
├── Makefile               # Build system
├── release/               # Stable working binaries (ALWAYS WORKS)
│   └── git-prompt         # Last known good build
└── target/                # Build output directory
    ├── git-prompt                  # Main binary (shipping)
    ├── git-prompt-unpatched        # Baseline binary
    ├── git-prompt-patched-debug    # Debug binary
    ├── git-prompt-asan             # Address Sanitizer binary
    └── git-prompt-ubsan            # Undefined Behavior Sanitizer binary
```

## Key Architecture

### Code Organization (git-prompt.c)

The code is organized into logical sections:

1. **Configuration & Globals** (lines 1-82)
   - ANSI color codes, performance thresholds
   - Global flags: `use_color`, `debug_mode`, `large_repo_size`, `local_mode`
   - Usage strings and help text

2. **Data Structures** (lines 194-199)
   - `struct git_state`: Holds git operation state (merge/rebase/cherry-pick/conflicts)
   - Used to pass state information between functions

3. **Helper Functions** (lines 144-398)
   - `color_printf()`: ANSI color output
   - `is_large_repo()`: Performance optimization check
   - `has_unmerged_files()`: Detect conflicts (unmerged index entries)
   - `has_staged_changes()`: Detect if index differs from HEAD (takes git_state)
   - `has_worktree_changes()`: Working tree status detection
   - `get_git_state()`: Detect merge/rebase/cherry-pick states (returns struct)

4. **BFS Divergence Algorithm** (lines 400-600)
   - Bidirectional BFS to find commit divergence
   - Ring buffer queue implementation for performance
   - Returns ahead/behind counts between two commits

5. **Prompt Generation Sections** (lines 600-1200)
   - Section 1: `get_branch_name_and_color()` - Branch name and working tree color
   - Section 2: `get_tracking_indicators()` - Upstream tracking and divergence from main
   - Section 3: `get_misc_indicators()` - Detached HEAD, git state, stash

6. **Main Entry Point** (lines 1200-1300)
   - Argument parsing
   - Git repository setup
   - Config loading (skipped with `--local` flag)
   - Orchestrates the three sections above in order:
     1. Get git state FIRST (for conflict detection)
     2. Get branch name and color (using state)
     3. Get tracking indicators
     4. Get misc indicators (using state)

### Important Global Variables

- `local_mode`: When set (via `--local` flag), skips reading global git config. **Always use this in tests** to avoid global config interference.
- `debug_mode`: Enables timing output to stderr
- `large_repo_size`: Index size threshold (default 5MB) for skipping expensive status checks

## Build System

### Makefile Targets

The Makefile builds **five binaries** for testing purposes:

1. **git-prompt-unpatched** - Baseline from upstream git
2. **git-prompt** - Main shipping binary (with local patches)
3. **git-prompt-patched-debug** - Debug version with timing enabled
4. **git-prompt-asan** - Address Sanitizer build for memory error detection
5. **git-prompt-ubsan** - Undefined Behavior Sanitizer build

All binaries are placed in `target/` directory.

### Build Process

```bash
make           # Build all five binaries
make test      # Run test suite (auto-discovers all binaries)
make asan      # Run Address Sanitizer tests
make ubsan     # Run Undefined Behavior Sanitizer tests
```

The build uses libgit.a from the git submodule, so the git submodule must be initialized and built first.

### IMPORTANT: Avoid `make clean`

**DO NOT run `make clean` unless absolutely necessary.** It rebuilds the entire git submodule which is extremely slow.

Instead, if you need to rebuild something specific:

```bash
# If only git-prompt.c changed, just rebuild the specific binary:
rm target/git-prompt.o target/git-prompt

# If you need to rebuild all five binaries:
rm target/git-prompt*.o target/git-prompt*

# Only use make clean if you're certain the build system is broken
```

The build system is incremental - it only rebuilds what changed. Trust it unless you have a specific reason not to.

### Release Directory

The `release/` directory contains the **last known good build** that always works. If you break something during development, you can always fall back to `release/git-prompt`.

## Testing

### Test System

Tests are defined declaratively in `tests/test_cases.yaml` and executed by `tests/run_tests.py`.

#### Test Structure

Each test case has:
- `name`: Test description
- `group`: Category (basic, working-tree, branches, upstream, etc.)
- `reset`: (optional) Start with fresh git repo
- `steps`: List of shell commands to set up state (can use dict with `command` and `repeat` for repeated commands)
- `expected`: Expected output with color markers like `{GREEN}[master]{}`
- `large_repo_size`: (optional) Override size threshold

**Repeated Commands**: To avoid verbose test output, use dict format for repeated commands:
```yaml
steps:
  - command: echo x >> file.txt && git add file.txt && git commit -m 'commit'
    repeat: 100
```
This shows as "Repeated 100 times:" in reports instead of 100 individual lines.

#### Running Tests

```bash
cd tests
./run_tests.py                # Auto-discovers all binaries, always generates reports
./run_tests.py --verbose      # Show detailed output
```

The test runner:
- Auto-discovers all five binaries from `../target/` directory (relative to tests/)
- Always generates HTML and text reports in tests/ directory

**IMPORTANT**: Tests always use `--local` flag (hardcoded in `get_git_prompt_output()`) to avoid global git config interference.

#### Test Reports

The test runner generates three types of reports:

1. **Text Report** (`test-results.txt`): Plain text format with escaped color markers like `{GREEN}[master]{}`. Shows both expected patterns and actual output for all tests. Repeated commands are shown compactly as "Repeated N times:".

2. **Detailed HTML Report** (`test-results.html`): Interactive HTML with collapsible sections showing all setup steps and results for each test. Failed tests are expanded by default.

3. **Summary HTML Report** (`examples.html`): Grouped examples of git-prompt output by category (basic, working-tree, branches, etc.).

### Test Pattern Variables

Expected output supports pattern matching:
- `$COMMIT` - Any 7-character hex hash
- `$NUMBER` - Any integer
- `$ANYTHING` - Any non-whitespace string

Example: `"{GREEN}[$COMMIT]{} {MAGENTA}⚡{}"`

## Command-Line Flags

```
git prompt [options]

--no-color              Disable colored output
--debug                 Show timing information (stderr)
--large-repo-size=N     Set index size threshold (default: 5000000 bytes)
--local                 Skip reading global git config (use in tests!)
--help                  Show help message
```

## Development Workflow

### Making Changes

1. Edit `git-prompt.c`
2. **Do NOT run `make` yourself** - let the user build
3. Test changes: User will run `make && make test`

### Adding Tests

1. Edit `tests/test_cases.yaml`
2. Add test case with appropriate `group` and `expected` output
3. Use `reset: true` if test needs clean git repo
4. Tests automatically use `--local` flag

### Understanding Output Colors

- **Branch Colors** (main indicator):
  - GREEN: Clean working tree (no changes, nothing staged)
  - YELLOW: Staged changes (ready to commit)
  - RED: Unstaged changes (need to stage or commit)
  - CYAN: Untracked files only
  - GRAY: **Performance fallback** - Large repo (expensive status checks skipped)

- **Indicator Colors**:
  - BLUE: Ahead (should push)
  - YELLOW: Behind (should pull)
  - RED: Diverged or conflicts
  - MAGENTA: Detached HEAD
  - CYAN: Stash or operations in progress

**Gray Color Philosophy**: GRAY is a fallback when we choose not to perform a full scan because it would be too slow. It indicates "we don't know the exact state, but the repo is still functional." This keeps prompts fast even in large repositories.

## Git Config Handling

### The --local Flag

The `--local` flag was added specifically for testing to prevent global git config from interfering with test results.

**When --local is set**:
- Skips `repo_config()` call in main()
- No global excludes loaded (no `~/.gitignore_global`)
- No user-specific git config applied
- Tests remain isolated and reproducible

**Code location**: git-prompt.c:1079-1083

## Common Patterns

### Reading the Code

When exploring git-prompt.c:
1. Start with `main()` (line ~1200) to see the flow
2. Understand the execution order:
   - Git state detection runs FIRST (to detect conflicts)
   - Branch color determination (uses git_state)
   - Tracking indicators
   - Misc indicators (uses git_state)
3. Key architectural pattern: `struct git_state` is passed to functions that need state info
4. BFS algorithm is self-contained (lines ~400-600)
5. Helper functions are used throughout

### Debugging

Use `--debug` flag to see timing breakdown:
```bash
git prompt --debug
```

This shows timing for:
- Config load
- Index load
- Branch name detection
- Status checks
- Divergence calculation

## Performance Considerations

### Large Repo Mode

When `.git/index` size exceeds `large_repo_size` threshold (default 5MB), git-prompt enters **large repo mode**:

- Branch color becomes GRAY (performance fallback)
- Expensive operations are skipped (worktree scanning, staged change detection)
- Fast operations still run (branch name, tracking info, stash count)

**Design Philosophy**: GRAY indicates "we're choosing not to scan because it's slow" - it's an intentional performance optimization, not an error state. The goal is to keep the prompt responsive even in massive repositories.

### Other Optimizations

- **BFS Limit**: Divergence calculation stops at 100 commits (MAX_TRAVERSAL_DEFAULT)
- **Index Caching**: Index loaded once at startup, reused throughout
- **Result Caching**: Divergence results cached in `.git/prompt-cache` when traversal cost >= 10 commits

### Function Performance Characteristics

See code comments in git-prompt.c for detailed complexity analysis of each function. Key categories:

- **O(1) - Constant time**: Safe to call in large repo mode (config reads, state file checks)
- **O(n) where n = index entries**: Scanning the index (can be expensive in large repos)
- **O(m) where m = worktree files**: Filesystem operations (expensive in large repos)
- **O(commits)**: Graph traversal for divergence (limited by MAX_TRAVERSAL)

## Important Notes

- Git config is loaded in main() (skipped with `--local`)
- Tests MUST use `--local` to avoid global config interference
- The `local_mode` variable exists at line ~64
- Do NOT run `make` or build commands unless explicitly asked by the user
- Five binaries are built: unpatched (baseline), main (shipping), patched-debug (analysis), asan, and ubsan
- The test runner auto-discovers all binaries and validates they all produce identical output

## Key Architectural Decisions

### struct git_state Pattern

The v4 implementation introduced `struct git_state` to cleanly pass git operation state between functions:

```c
struct git_state {
    int has_state;          /* 1 if any git operation is in progress */
    int has_conflicts;      /* 1 if unmerged files exist */
    const char *state_name; /* e.g., "merge:conflict", "rebase:continue" */
    const char *state_color;/* Color for the state indicator */
};
```

**Why this matters**:
- `get_git_state()` runs FIRST to detect conflicts
- `has_staged_changes()` checks `state->has_conflicts` BEFORE cache-tree comparison
- During conflicts, unmerged entries count as staged changes (YELLOW branch color)
- Same struct is reused for displaying `[merge:conflict]` indicator
- Uses compile-time constant strings (no memory management needed)

**Code locations**:
- `struct git_state` definition: line 194-199
- `get_git_state()`: line 357-398 (returns struct with flags)
- `has_staged_changes()`: line 217-290 (checks conflicts first, line 233)
- Main execution order: lines 1228-1242 (state computed first)
