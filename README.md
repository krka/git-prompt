# git-prompt

A fast, standalone git repository status tool optimized for shell prompt integration.

**📖 [Full Documentation](https://krka.github.io/git-prompt/)** - Examples, usage guide, and color reference

## Features

- **Fast**: Interleaved bidirectional BFS for efficient divergence calculation (~5ms typical)
- **Informative**: Shows branch, ahead/behind counts, working tree status, and git state
- **Colorful**: Color-coded output for quick visual scanning
- **Standalone**: Separate repository that links against git as a submodule

## Installation

### Building from Source

Clone the repository with submodules:

```bash
git clone --recursive https://github.com/krka/git-prompt
cd git-prompt
make
```

If you cloned without `--recursive`, initialize the submodule:

```bash
git submodule update --init --recursive
make
```

The build process:
1. Initializes the git submodule (if needed)
2. Builds git's libgit.a and supporting libraries (~30s on first build)
3. Compiles and links git-prompt (~1s)

Subsequent builds are fast since git's libraries are only rebuilt when needed.

### Shell Integration

After building, integrate `git-prompt` into your shell prompt:

**Bash:**

Add to your `~/.bashrc`:

```bash
# Add git-prompt to your existing PS1
PS1='$(/path/to/git-prompt)'"\$PS1"
```

Or create a custom prompt from scratch:

```bash
PS1='$(/path/to/git-prompt)\w\$ '
```

Replace `/path/to/git-prompt` with the actual path to the binary.

**Zsh:**

Add to your `~/.zshrc`:

```zsh
setopt PROMPT_SUBST
PROMPT='$(/path/to/git-prompt)%~ %# '
```

**Fish:**

Add to your `~/.config/fish/config.fish`:

```fish
function fish_prompt
    /path/to/git-prompt
    echo -n (prompt_pwd)' > '
end
```

## Usage

Run directly:

```bash
./git-prompt
```

### Options

- `--help`, `-h`: Show help message
- `--no-color`: Disable colored output
- `--debug`: Show timing information for performance analysis
- `--large-repo-size=<bytes>`: Set index size threshold for large repo detection (default: 5000000)
- `--max-traversal=<commits>`: Maximum commits to traverse in divergence calculation (default: 1000)
- `--local`: Skip reading global git config (useful for testing)

## Output Format

```
[branch] indicators
```

The tool provides **two levels of tracking** to help you understand your branch status:

1. **Main branch divergence** (without parentheses): Shows how far your branch has diverged from `origin/main` or `origin/master`
2. **Upstream tracking** (in parentheses): Shows sync status with your configured upstream branch (what you've pushed)

### Branch Colors

- **Green**: Clean working tree
- **Cyan**: Untracked files only
- **Yellow**: Unstaged changes
- **Red**: Staged changes (ready to commit)
- **Gray**: Large repository (status check skipped)

### Indicators

**Main Branch Divergence** (shown without parentheses):
- `↑N` - N commits ahead of origin/main or origin/master
- `↓N` - N commits behind origin/main or origin/master
- `↑N↓M` - Diverged (N ahead, M behind origin/main or origin/master)
- `↕` - Too far diverged from main (exceeds --max-traversal limit)

**Upstream Tracking** (shown in parentheses, for branches with configured upstream):
- `(↑N)` - N commits ahead of upstream (ready to push)
- `(↓N)` - N commits behind upstream (need to pull)
- `(↑N↓M)` - Diverged from upstream (N ahead, M behind)
- `(↕)` - Too far diverged from upstream (exceeds --max-traversal limit)
- _(nothing shown when in sync with upstream)_

**Other Indicators**:
- `○` - No upstream configured
- `⚡` - Detached HEAD
- `[state]` - Git operation in progress (merge, rebase, cherry-pick, revert)
  - Red when conflicts present, cyan otherwise
- `💾` - Stashed changes present

### Examples

```
[main]                  # On main, in sync with upstream, clean working tree
[main] (↑2)             # On main, 2 commits ahead of upstream (need to push)
[feature] ○             # On feature branch, no upstream configured
[feature] ↑5            # Feature branch: 5 commits ahead of main, synced with upstream
[feature] ↑5↓3          # Feature branch: 5 ahead/3 behind main, synced with upstream
[feature] ↑10(↑2)       # Feature: 10 ahead of main, 2 unpushed commits to upstream
[feature] ↑5(↓1)        # Feature: 5 ahead of main, 1 commit behind upstream (need to pull)
[main] ⚡               # Detached HEAD on main
[main] [merge:conflict] # Merge in progress with conflicts (red)
[feature] 💾            # Feature branch with stashed changes
```

## Performance

The tool uses an optimized interleaved bidirectional BFS algorithm to calculate
divergence from upstream and origin branches. Typical performance:

- **Small repos**: 3-5ms total
- **Large repos** (like git.git): 15-20ms total
- **BFS divergence check**: <1ms (vs 15-20ms with sequential two-phase approach)

Performance optimizations:
- Single-pass interleaved BFS instead of two sequential passes
- Ring buffer queue for cache-friendly traversal
- Shared distance map for visited nodes
- Early termination on intersection detection
- Traversal limit of 1000 commits by default (configurable via --max-traversal)
- Intelligent caching system (stores results when BFS visits ≥10 commits)

### Caching System

The tool implements an intelligent caching mechanism to avoid redundant BFS traversals:

- **Cache location**: `.git/prompt-cache`
- **Cache key**: Based on HEAD, remote branch, and upstream OIDs
- **Cache policy**: Only caches when BFS visits ≥10 commits (avoids caching trivial cases)
- **Format**: `<head_oid>,<remote_oid>,<tracking_oid>=<main_ahead>,<main_behind>,<upstream_ahead>,<upstream_behind>`
- **Invalidation**: Automatic when any of the commit OIDs change

The cache dramatically speeds up repeated prompt calls in the same git state.

## Tests

Run the test suite:

```bash
make test
# Or directly:
cd tests
python3 run_tests.py
```

This runs all test cases from `test_cases.yaml` and automatically generates:
- Pass/fail results for each test case
- Three reports in tests/ directory:
  - `examples.html` - Visual examples grouped by category
  - `test-results.html` - Detailed results with all test steps
  - `test-results.txt` - Plain text report

Use `--verbose` to see detailed output and `--replace-expected` to update test expectations after behavior changes.

## Implementation Details

### Key Algorithm: Interleaved Bidirectional BFS

The tool uses an interleaved bidirectional BFS to find the divergence between
two commits (e.g., HEAD and origin/master):

1. **Initialization**: Two separate queues (one for each side) with shared distance map
2. **Traversal**: Round-robin alternation between queues, processing one commit from each side
3. **Distance Tracking**: Shared hashmap tracks `dist_from_start` and `dist_from_target` for each commit
4. **Intersection Detection**: When a commit is visited that already has a distance from the other side, we've found the merge-base
5. **Result**: `ahead = dist_from_start`, `behind = dist_from_target` at intersection point

This approach is significantly faster than the traditional two-phase approach:
- **Old**: Build full distance map from target (phase 1), then search from start (phase 2)
- **New**: Both searches progress together in perfect interleaving, meeting in the middle

Benefits:
- ~10x fewer commits visited for typical cases
- ~2-3x faster overall
- More cache-friendly (single map instead of separate visited sets)
- Early termination as soon as merge-base is found

### File Structure

- `git-prompt.c` - Main implementation (~1150 lines)
- `Makefile` - Build configuration
- `submodules/git/` - Git source code (submodule)
- `tests/` - Test suite
  - `run_tests.py` - Python test runner
  - `test_cases.yaml` - Declarative test cases
  - `*.html`, `*.txt` - Generated test reports (git-ignored)
- `README.md` - This file

## License

Same as Git (GPLv2)
