# Development Guide

This guide covers development workflows, testing, debugging, and understanding the codebase.

## Quick Start

```bash
# First time setup
git clone --recursive https://github.com/yourusername/git-prompt
cd git-prompt
make

# Run tests
make test
```

## Project Structure

```
git-prompt/
├── git-prompt.c           # Main source (~1150 lines)
├── Makefile               # Build system
├── submodules/git/        # Git source tree (submodule)
├── tests/
│   ├── run_tests.py       # Python test runner
│   ├── test_cases.yaml    # Declarative test cases
│   └── test-styles.css    # HTML report styles
├── tools/                 # Helper scripts for patching
├── docs/                  # Documentation
├── release/               # Stable binaries (always works)
└── target/                # Build output
    ├── git-prompt                   # Main shipping binary (~700KB)
    ├── git-prompt-unpatched         # Baseline reference (~11MB)
    └── git-prompt-patched-debug     # Debug binary (~11MB)
```

## Build System

### Three Binary Build Strategy

The Makefile builds **three binaries** for comparison testing:

1. **git-prompt-unpatched** - Baseline from upstream git (source of truth)
   - Built with standard flags: `-g -O2 -Wall`
   - Links against raw git libraries (no patches)
   - Size: ~11 MB (includes debug symbols)
   - **This is the reference**: If test outputs differ, this version is correct

2. **git-prompt** - Main shipping binary
   - Built with aggressive optimization: `-O2 -flto -ffunction-sections -fdata-sections`
   - Stripped: `-s -Wl,--gc-sections`
   - Links against patched git libraries (dead code removed)
   - Size: ~700 KB
   - **This is what users get**

3. **git-prompt-patched-debug** - Debug version with symbols
   - Built with optimization but keeps symbols: `-g -O2 -flto`
   - Links against patched git libraries
   - Size: ~11 MB
   - **Use for debugging patches and analyzing binary size**

### Build Targets

```bash
make              # Build all three binaries (default)
make test         # Build and run tests
make clean        # Remove build artifacts (preserves release/)
make distclean    # Deep clean including git submodule build
make install      # Install to /usr/local/bin (or PREFIX=/path)
make release      # Build, test, and hardlink to release/ if tests pass
make help         # Show all targets
```

### Important: Avoid `make clean`

**DO NOT run `make clean` unless absolutely necessary.** It rebuilds the entire git submodule which takes ~30 seconds.

The build system is incremental and only rebuilds what changed:

```bash
# If only git-prompt.c changed, rebuild is automatic and fast (<1s)
make

# If you need to force rebuild just the binaries:
rm target/git-prompt*.o target/git-prompt*
make

# Only use clean if the build system is broken
make clean
```

### Cached Git Libraries

Git libraries are built once and cached in `target/git/`:
- `target/git/raw/` - Unpatched libraries (for baseline binary)
- `target/git/patched/` - Patched libraries (for optimized binaries)

These are only rebuilt when:
- Cache doesn't exist
- Patches change (`tools/patches.yaml` modified)
- You run `make clean`

## Development Workflow

### Making Code Changes

```bash
# 1. Edit git-prompt.c
vim git-prompt.c

# 2. Rebuild (automatic, fast)
make

# 3. Run tests
make test

# 4. If tests pass, update release binary
make release
```

### Running Individual Tests

```bash
# Run all tests with HTML report
cd tests
./run_tests.py --git-prompt=../target/git-prompt

# Verbose output (shows all git commands)
./run_tests.py --verbose

# Run and update expectations after behavior change
./run_tests.py --replace-expected

# Skip HTML generation
./run_tests.py --no-html
```

### Adding New Tests

Edit `tests/test_cases.yaml`:

```yaml
- name: "Description of test"
  group: "category"  # basic, working-tree, branches, upstream, etc.
  reset: true        # Start with fresh git repo (optional)
  steps:
    - git init
    - git config user.name "Test"
    - git config user.email "test@example.com"
    - echo "content" > file.txt
    - git add file.txt
    - git commit -m "Commit"
  expected: "{GREEN}[master]{}"
```

**Pattern Variables** in `expected`:
- `$COMMIT` - Any 7-character hex hash
- `$NUMBER` - Any integer
- `$ANYTHING` - Any non-whitespace string

**Repeated Commands** (for performance tests):
```yaml
steps:
  - command: echo x >> file.txt && git add file.txt && git commit -m 'commit'
    repeat: 15
```

**Important**: Tests automatically use `--local --max-traversal=10` to:
- Avoid global git config interference
- Keep tests fast (default is 1000 commits in production)

## Debugging

### Debug Mode

```bash
# Show timing breakdown
./target/git-prompt --debug

# Example output:
# [DEBUG] Config load: 0.234ms
# [DEBUG] Index load: 1.456ms
# [DEBUG] Branch name: 0.123ms
# [DEBUG] Status: change check: 2.345ms
# [DEBUG] Divergence check: 3.456ms
# [DEBUG] Total: 7.614ms
```

### Understanding Test Failures

When tests fail, check:

1. **Which binary failed?**
   - If unpatched fails: Bug in git-prompt.c (fix the code)
   - If patched fails but unpatched passes: Patch issue (check tools/patches.yaml)
   - If all three fail: Test expectation wrong (use --replace-expected)

2. **Test reports**:
   - `tests/test-results.txt` - Plain text with escaped color codes
   - `tests/test-results.html` - Interactive HTML (failed tests expanded)
   - `tests/examples.html` - Visual examples grouped by category

3. **Verbose mode**:
   ```bash
   cd tests
   ./run_tests.py --verbose
   ```

### Performance Profiling

```bash
# Basic timing
./target/git-prompt --debug

# Detailed profiling with perf
perf record -g ./target/git-prompt
perf report

# Check binary size after changes
ls -lh target/git-prompt*
```

### Analyzing Binary Size

```bash
# Compare sizes
ls -lh target/git-prompt*

# Detailed analysis
./tools/analyze-binary.sh target/git-prompt
./tools/analyze-binary.sh target/git-prompt-patched-debug

# See which functions are included
nm -S --size-sort target/git-prompt | less
```

## Static Analysis & Code Quality

The project includes several static analysis tools to automatically find bugs, memory leaks, and code quality issues.

### Prerequisites

Install the required tools:

```bash
# Ubuntu/Debian
sudo apt-get install cppcheck clang-tidy valgrind

# macOS
brew install cppcheck llvm valgrind
```

### Available Analysis Tools

#### `make analyze`

Runs static code analyzers (cppcheck + clang-tidy) to find potential bugs and style issues.

**What it checks:**
- Potential null pointer dereferences
- Unchecked return values
- Memory leaks
- Unused variables
- Suspicious code patterns
- Style inconsistencies

**Example usage:**
```bash
make analyze

# Example output:
# === cppcheck ===
# [git-prompt.c:1102]: (warning) Possible null pointer dereference: main_branch_allocated
#
# === clang-tidy ===
# git-prompt.c:722:5: warning: magic number detected [readability-magic-numbers]
```

**Configuration:** Edit `.clang-tidy` to customize checks.

#### `make valgrind`

Runs valgrind memory leak detection on the debug binary.

**What it checks:**
- Memory leaks (allocated but not freed)
- Use of uninitialized memory
- Invalid memory access
- Double frees

**Example usage:**
```bash
make valgrind 2>&1 | tee valgrind.log
less valgrind.log
```

**Note:** Valgrind is slow (10-50x slowdown) but very thorough. It runs the program in the current git repository.

**Configuration:** Edit `.valgrind.supp` to suppress known issues from git libraries.

#### `make asan`

Builds and runs with Address Sanitizer (ASan) to detect memory errors.

**What it checks:**
- Use-after-free
- Heap buffer overflows
- Stack buffer overflows
- Memory leaks (when ASAN_OPTIONS=detect_leaks=1)

**Advantages over valgrind:**
- Much faster (2-3x slowdown vs 10-50x for valgrind)
- Better error reporting with exact line numbers
- Catches different types of errors

**Example usage:**
```bash
make asan

# Example output:
# =================================================================
# ==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x...
# READ of size 1 at 0x... thread T0
#     #0 0x... in get_branch_name git-prompt.c:845
```

#### `make ubsan`

Builds and runs with Undefined Behavior Sanitizer (UBSan) to detect undefined behavior.

**What it checks:**
- Integer overflow/underflow
- Division by zero
- Null pointer dereference
- Type mismatches
- Out-of-bounds array access

**Example usage:**
```bash
make ubsan

# Example output:
# git-prompt.c:122:5: runtime error: signed integer overflow: 2147483647 + 1
```

#### `make check-all`

Runs all static analysis tools in sequence: `analyze`, `asan`, and `ubsan`.

**Recommended usage before committing significant changes:**
```bash
make check-all
```

### Common Workflows

#### Before Committing

Run the full test suite and static analysis:

```bash
make clean
make all
make test
make check-all
```

#### Debugging Memory Issues

If tests are crashing or behaving strangely:

```bash
# Quick check with ASan (fast, 2-3x slowdown)
make asan

# Thorough check with valgrind (slow but comprehensive)
make valgrind 2>&1 | tee valgrind.log
less valgrind.log
```

#### Finding Code Quality Issues

```bash
# Static analysis (no execution required)
make analyze

# Check for undefined behavior
make ubsan
```

### Configuration Files

#### `.clang-tidy`

Configuration for clang-tidy static analyzer.

**Enabled checks:**
- `bugprone-*` - Bug-prone pattern detection
- `clang-analyzer-*` - Clang static analyzer checks
- `performance-*` - Performance improvements
- Select readability checks

**Disabled checks:**
- `bugprone-easily-swappable-parameters` - Too noisy
- `readability-magic-numbers` - Legitimate magic numbers exist

**Customization:** Edit `.clang-tidy` to add/remove checks.

#### `.valgrind.supp`

Valgrind suppressions for known issues in third-party libraries.

**Usage:** Add suppressions here for issues you can't fix (e.g., git library internals):

```
{
   git_config_leak
   Memcheck:Leak
   ...
   fun:git_config_*
}
```

### Integration with Development Workflow

```bash
# 1. Make changes
vim git-prompt.c

# 2. Quick rebuild and test
make test

# 3. Static analysis (before commit)
make analyze

# 4. Memory checks (if changing allocation/pointers)
make asan
make ubsan

# 5. Thorough check (before major releases)
make valgrind
```

## Code Architecture

### Main Execution Flow (git-prompt.c)

```c
main()
  ├─ Parse arguments
  ├─ Load git config (unless --local)
  ├─ Load git index (cached)
  ├─ Get git state (merge/rebase/conflicts)    // Section 0
  ├─ Get branch name and color                 // Section 1
  │   ├─ Detect detached HEAD
  │   ├─ Check working tree changes
  │   └─ Check staged changes (uses git_state)
  ├─ Get misc indicators                       // Section 3
  │   ├─ Detached HEAD indicator
  │   ├─ Git state indicator (uses git_state)
  │   └─ Stash indicator
  ├─ Get tracking indicators                   // Section 2
  │   ├─ BFS: divergence from origin/main
  │   └─ BFS: divergence from upstream
  └─ Print output
```

### Key Data Structures

```c
struct git_state {
    int has_state;          // 1 if git operation in progress
    int has_conflicts;      // 1 if unmerged files exist
    const char *state_name; // e.g., "merge:conflict"
    const char *state_color;// Color for indicator
};

struct prompt_context {
    struct object_id oid;   // HEAD commit
    struct ref_store *refs; // Ref store
    int large_repo;         // Large repo flag
    int index_loaded;       // Index loaded flag
};
```

### Important Architectural Decisions

1. **Git state computed first**: `get_git_state()` runs before color determination because conflicts affect staged status
2. **Conflicts count as staged**: Unmerged entries are considered staged changes (YELLOW branch)
3. **Cache-tree comparison**: `has_staged_changes()` uses cache-tree OID comparison for speed
4. **BFS interleaving**: Bidirectional BFS alternates between queues for optimal performance

### BFS Algorithm (Lines 481-640)

The core divergence calculation uses interleaved bidirectional BFS:

```
Two queues (start, target) + shared distance map
│
├─ Round-robin: Process one commit from each queue
├─ Track distances from both sides
├─ Detect intersection: When commit has distance from both sides
└─ Result: ahead = dist_from_start, behind = dist_from_target
```

See `docs/OPTIMIZATION-REPORT.md` for detailed performance analysis.

## Testing Strategy

### Test Groups

- `basic` - Repository initialization, first commit
- `working-tree` - Clean, modified, staged, untracked states
- `branches` - Branch creation, switching
- `detached` - Detached HEAD scenarios
- `upstream` - Tracking branches, ahead/behind
- `stash` - Stashed changes
- `in-progress` - Merge, rebase, cherry-pick states

### Test Runner Architecture

The test runner (`run_tests.py`):
1. Discovers all three binaries automatically
2. Runs each test against all binaries
3. Compares outputs (unpatched is source of truth)
4. Generates three reports (text, detailed HTML, summary HTML)

### Test Flags

Tests automatically use these flags:
- `--local` - Skip global git config (isolation)
- `--max-traversal=10` - Fast tests (vs 1000 in production)

Individual tests can override:
```yaml
- name: "Custom test"
  large_repo_size: 100     # Override repo size threshold
  max_traversal: 5         # Override traversal limit
```

## Common Tasks

### Update Upstream Git Version

```bash
cd submodules/git
git fetch
git checkout v2.43.0  # Or desired version
cd ../..
git add submodules/git
git commit -m "Update git submodule to v2.43.0"
make clean  # Required to rebuild libraries
make test
```

### Add New Color Code

1. Add color definition in git-prompt.c (~line 40)
2. Update `ansi_to_markers()` in run_tests.py (~line 92)
3. Update `ansi_to_html()` in run_tests.py (~line 136)
4. Add tests for the new color

### Modify Patches

See `tools/PATCHING.md` for details on the patching system.

```bash
# Edit patches
vim tools/patches.yaml

# Rebuild (automatically applies patches)
make clean
make
```

## Performance Tips

1. **Index is loaded once**: Reused throughout execution
2. **Large repo detection**: Skips expensive checks when `.git/index` > 5MB
3. **Caching**: Results cached when BFS visits ≥10 commits
4. **Early termination**: BFS stops as soon as merge-base is found

## Release Process

```bash
# 1. Make changes and test
make test

# 2. Build release binary (hardlinked if tests pass)
make release

# 3. The release/ directory contains last known good build
ls -lh release/git-prompt
```

The `release/` directory:
- Never cleaned by `make clean`
- Only updated by `make release` when tests pass
- Provides a stable fallback if you break something

## Troubleshooting

### Build fails with "submodule not initialized"

```bash
git submodule update --init --recursive
make
```

### Tests fail with "Binary not found"

```bash
make  # Build first
make test
```

### Binary size increased unexpectedly

```bash
# Compare with previous version
ls -lh target/git-prompt
git show HEAD:git-prompt.c > /tmp/old.c
nm -S --size-sort target/git-prompt | less
./tools/analyze-binary.sh target/git-prompt
```

### Tests are slow

Tests use `--max-traversal=10` by default (vs 1000 in production). If still slow:
- Check if you have many repeated commands (>15 iterations)
- Consider splitting into multiple smaller tests
- Use `reset: true` sparingly (git init is expensive)

## Git Hooks (Optional)

Add to `.git/hooks/pre-commit`:

```bash
#!/bin/bash
make test
```

```bash
chmod +x .git/hooks/pre-commit
```

## Resources

- Main documentation: `README.md`
- Optimization report: `docs/OPTIMIZATION-REPORT.md`
- Patching system: `tools/PATCHING.md`
- Dynamic dispatch analysis: `docs/dynamic-dispatch-tables.md`
- Code guide: `CLAUDE.md` (detailed architecture notes)
