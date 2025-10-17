# git-prompt Improvement Roadmap

**Last Updated**: 2025-10-16 (formatting standardization complete)

This document tracks potential improvements to the git-prompt project, organized by category and priority.

---

## Quick Reference

- **Total Items**: 58
- **Completed**: 4 (clang-format setup, initial static analysis)
- **High Priority**: 15
- **Medium Priority**: 23
- **Low Priority**: 20

---

## 1. Code Structure & Organization

### A. Modularity (High Priority)

- [ ] **Split git-prompt.c into separate compilation units**
  - [ ] Create `src/main.c` - Entry point, arg parsing
  - [ ] Create `src/git_state.c/.h` - State detection logic (lines 399-510)
  - [ ] Create `src/branch.c/.h` - Branch name/color (lines 774-898)
  - [ ] Create `src/divergence.c/.h` - BFS algorithm (lines 598-757)
  - [ ] Create `src/tracking.c/.h` - Tracking indicators (lines 1053-1267)
  - [ ] Create `src/indicators.c/.h` - Misc indicators (lines 1278-1296)
  - [ ] Create `src/cache.c/.h` - Divergence caching (lines 901-1038)
  - [ ] Create `src/util.c/.h` - Color helpers, common utilities
  - [ ] Update Makefile to build multiple object files
  - [ ] Update documentation to reflect new structure

### B. Error Handling (Medium Priority)

- [ ] **Add return codes for critical functions**
  - [ ] `repo_read_index()` failures (git-prompt.c:294, 368, 379, 1385)
  - [ ] `repo_parse_commit()` failures (git-prompt.c:312)
  - [ ] `lookup_commit()` failures (git-prompt.c:311, 686, 793)
- [ ] **Add graceful degradation when git operations fail**
- [ ] **Create optional verbose error mode** (`--verbose-errors` flag)
- [ ] **Add error logging to stderr with context**

### C. Magic Numbers (Low Priority)

- [ ] **Extract hardcoded values to named constants**
  - [ ] BFS queue full check (line 722: `BFS_QUEUE_SIZE - 1`)
  - [ ] Cache write threshold (line 1007: `< 10`)
  - [ ] Color code definitions to dedicated header
  - [ ] String buffer sizes

---

## 2. Build System

### A. Dependency Tracking (Medium Priority)

- [ ] **Add automatic header dependency tracking**
  - [ ] Use `-MMD -MP` flags for dependency generation
  - [ ] Include generated `.d` files in Makefile
  - [ ] Test incremental builds with header changes

### B. Configuration Options (Low Priority)

- [ ] **Create `config.mk` for user customization**
  - [ ] Allow CC override
  - [ ] Allow CFLAGS customization
  - [ ] Allow PREFIX override
  - [ ] Document in README.md

### C. Build Improvements (Low Priority)

- [ ] **Add binary size verification to `make all`**
- [ ] **Add `make install-local` target** (install to `~/.local/bin`)
- [ ] **Add `make format` target** (clang-format) - `.clang-format` configuration exists, just needs Makefile target
- [ ] **Document Makefile help is incomplete** (missing test-update target)

---

## 3. Test Suite

### A. Test Coverage Gaps (High Priority)

- [x] **Add edge case tests**
  - [x] Rebase with conflicts
  - [ ] Repositories with submodules
  - [x] Git worktree scenarios
  - [x] Git bisect operations
  - [x] Git apply/am operations
  - [ ] Cache invalidation behavior
  - [ ] Corrupted git repository handling
  - [ ] Very large divergence (>10000 commits)

### B. Test Organization (Medium Priority)

- [ ] **Add test categories to test_cases.yaml**
  - [ ] Smoke tests category
  - [ ] Regression tests category
  - [ ] Edge cases category
  - [ ] Performance tests category
- [ ] **Add `--category` flag to run_tests.py**
- [ ] **Create `tests/unit/` for unit tests** (when code is modularized)

### C. Performance Benchmarking (Medium Priority)

- [ ] **Create `tests/benchmark.py`**
  - [ ] Benchmark BFS divergence calculation
  - [ ] Benchmark working tree status checks
  - [ ] Benchmark cache read/write
  - [ ] Track performance over time
  - [ ] Assert against thresholds
- [ ] **Add performance regression tests to CI**

---

## 4. Documentation

### A. User Documentation (High Priority)

- [ ] **Create `docs/TROUBLESHOOTING.md`**
  - [ ] Common issue: Prompt shows gray even for small repos
  - [ ] Common issue: Colors don't work in shell
  - [ ] Common issue: Slow performance in monorepo
  - [ ] Common issue: Detached HEAD confusion
  - [ ] Common issue: ↕ symbol meaning
- [ ] **Create FAQ section**
  - [ ] What does gray mean?
  - [ ] What's the difference between ↑N and (↑N)?
  - [ ] How do I customize colors?
  - [ ] When should I adjust --large-repo-size?
- [ ] **Document shell-specific gotchas**
  - [ ] Bash vs Zsh escaping differences
  - [ ] Fish shell integration details
  - [ ] ANSI color compatibility issues

### B. Architecture Documentation (Medium Priority)

- [ ] **Create `docs/ARCHITECTURE.md`**
  - [ ] Data flow diagram (ASCII art)
  - [ ] Call graph for main execution path
  - [ ] Memory layout and allocation strategy
  - [ ] BFS algorithm visualization
  - [ ] Cache format and invalidation strategy
  - [ ] Large repo mode decision tree

### C. Examples Gallery (Low Priority)

- [ ] **Create `docs/examples/` directory**
  - [ ] Screenshots of real-world usage
  - [ ] `bash/` - Bash integration examples
  - [ ] `zsh/` - Zsh integration examples
  - [ ] `fish/` - Fish integration examples
  - [ ] Integration with starship
  - [ ] Integration with oh-my-zsh
  - [ ] Integration with powerlevel10k

### D. GitHub Pages (Medium Priority)

- [ ] **Create Makefile target for GitHub Pages generation**
  - [ ] `make gh-pages` - Generate static HTML documentation
  - [ ] Convert README.md to HTML with styling
  - [ ] Include test examples HTML report
  - [ ] Include architecture diagrams (when available)
  - [ ] Add custom CSS for GitHub Pages theme
  - [ ] Generate table of contents
- [ ] **Create `tools/deploy-gh-pages.sh` script**
  - [ ] Build all documentation
  - [ ] Push to `gh-pages` branch
  - [ ] Verify deployment
- [ ] **Configure GitHub Pages settings**
  - [ ] Enable GitHub Pages in repository settings
  - [ ] Set custom domain (if desired)
  - [ ] Add CNAME file
- [ ] **Add documentation deployment to CI**
  - [ ] Auto-deploy on main branch updates
  - [ ] Verify links and images

---

## 5. Performance

### A. Profiling Infrastructure (Medium Priority)

- [ ] **Add built-in profiling support**
  - [ ] Create PROFILE macro for scope-based timing
  - [ ] Add --profile flag to output detailed timing
  - [ ] Profile BFS traversal
  - [ ] Profile cache operations
  - [ ] Profile index operations
- [ ] **Create profiling documentation**

### B. Memory Allocation (Low Priority)

- [ ] **Profile actual memory usage in real repositories**
- [ ] **Consider pre-allocated memory pool for BFS distance entries**
- [ ] **Consider arena allocator to avoid individual frees**
- [ ] **Benchmark memory usage in large repos**

### C. I/O Optimization (Low Priority)

- [ ] **Optimize cache file I/O**
  - [ ] Consider mmap() for cache reads
  - [ ] Consider compressing cache for large divergence sets
  - [ ] Benchmark cache performance
- [ ] **Reduce syscalls in large repo mode**

---

## 6. Features

### A. Enhanced Git State (Medium Priority)

- [ ] **Add git bisect state detection**
  - [ ] Show `[bisect: N/M]` during bisect
- [ ] **Add git am (apply mailbox) state**
  - [ ] Show `[am:continue]` or `[am:conflict]`
- [ ] **Show conflict count**
  - [ ] Display `[merge: 3 conflicts]` instead of just `[merge:conflict]`
- [ ] **Show rebase progress**
  - [ ] Display `[rebase: 2/5]` (current step / total steps)

### B. Configuration File Support (High Priority)

- [ ] **Design configuration file format**
  - [ ] Choose format: INI, YAML, or TOML
  - [ ] Define configuration schema
- [ ] **Implement configuration file parser**
  - [ ] Read `~/.git-promptrc` or `~/.config/git-prompt/config`
  - [ ] Read `.git/prompt-config` (repository-specific)
  - [ ] Merge global and local configs
- [ ] **Add configurable options**
  - [ ] Colors (clean, modified, staged, etc.)
  - [ ] Thresholds (large_repo_size, max_traversal)
  - [ ] Indicator symbols (detached_head, stash, ahead, behind)
  - [ ] Enable/disable specific features
- [ ] **Add `--show-config` flag** to display current configuration
- [ ] **Document configuration in README.md**

### C. Additional Indicators (Low Priority)

- [ ] **Add worktree count indicator**
  - [ ] Show `📁×3` if multiple worktrees exist
- [ ] **Add patch count in rebase**
  - [ ] Show remaining patches during rebase
- [ ] **Add git hooks status**
  - [ ] Warning indicator if pre-commit hook failed
- [ ] **Add ref log size indicator**
  - [ ] Indicator for unusually large reflog

### D. Other Features (Low Priority)

- [ ] **Add `--version` flag** (currently missing)
  - [ ] Define version numbering scheme
  - [ ] Display version and git submodule commit
- [ ] **Add `--format` flag** for custom output format
  - [ ] JSON output option
  - [ ] Templated output option

---

## 7. Project Organization

### A. Directory Structure (Medium Priority)

- [ ] **Reorganize project structure**
  - [ ] Create `src/` for source files (if modularized)
  - [ ] Create `include/` for public headers
  - [ ] Move existing docs to `docs/`
  - [ ] Create `examples/` directory
    - [ ] `examples/bash/`
    - [ ] `examples/zsh/`
    - [ ] `examples/fish/`
- [ ] **Update build system** for new structure
- [ ] **Update documentation** for new structure

### B. CI/CD (High Priority)

- [ ] **Create `.github/workflows/ci.yml`**
  - [ ] Build on Ubuntu, macOS, and Windows (WSL)
  - [ ] Run test suite
  - [ ] Run `make release` (ensure tests pass)
  - [ ] Check binary sizes
  - [ ] Upload artifacts
- [ ] **Add status badge to README.md**
- [ ] **Add code coverage reporting** (codecov or coveralls)

### C. Release Automation (Medium Priority)

- [ ] **Create `tools/release.sh` script**
  - [ ] Run all tests
  - [ ] Update version number
  - [ ] Build release binary
  - [ ] Create git tag
  - [ ] Generate release notes
  - [ ] Create GitHub release
- [ ] **Document release process**

---

## 8. Code Quality Tools

### A. Static Analysis (High Priority)

- [ ] **Add `make analyze` target**
  - [x] Run cppcheck
  - [x] Run clang-tidy
  - [ ] Document findings
  - [ ] Fix critical issues (5 real bugs identified)
- [ ] **Add static analysis to CI pipeline**
- [ ] **Create `.clang-tidy` configuration**

**Status**: Initial analysis completed. Used clang-tidy to add braces around single-line if statements (~75 instances). Applied clang-format to standardize code style. Identified 5 real bugs (enum casts, uninitialized variable, null pointer) and 53 fprintf security warnings to address.

### B. Code Coverage (Medium Priority)

- [ ] **Add `make coverage` target**
  - [ ] Build with coverage instrumentation
  - [ ] Run test suite
  - [ ] Generate coverage report with lcov
  - [ ] Generate HTML report
- [ ] **Set coverage goals** (aim for 80%+)
- [ ] **Add coverage to CI**

### C. Fuzzing (Low Priority)

- [ ] **Create `tests/fuzz/` directory**
- [ ] **Write fuzzing harness for git state parsing**
- [ ] **Write fuzzing harness for cache parsing**
- [ ] **Integrate with OSS-Fuzz or libFuzzer**
- [ ] **Document fuzzing process**

---

## 9. Quick Wins (Can Do Today)

- [x] **Add `.clang-format` file** for consistent code style
- [ ] **Create `CONTRIBUTING.md`** to encourage contributions
- [ ] **Add GitHub issue templates**
  - [ ] Bug report template
  - [ ] Feature request template
  - [ ] Question template
- [ ] **Fix Makefile help** - Add missing test-update target
- [ ] **Add version number** - Implement `--version` flag
- [ ] **Add LICENSE file** (currently mentions GPLv2 but no LICENSE file)
- [ ] **Add `.gitattributes`** for consistent line endings
- [ ] **Add EditorConfig** (`.editorconfig`) for consistent editor settings

---

## 10. Future Ideas (Backlog)

- [ ] **Plugin/extension system** for custom indicators
- [ ] **Remote repository status** (GitHub/GitLab PR status)
- [ ] **Commit signing status** indicator
- [ ] **Submodule status** in prompt
- [ ] **Git LFS status** indicator
- [ ] **Package for distribution**
  - [ ] Debian/Ubuntu package
  - [ ] Homebrew formula
  - [ ] AUR package (Arch Linux)
  - [ ] Snap package
- [ ] **Web-based configuration tool**
- [ ] **Integration tests with popular shells**

---

## Priority Summary

### Do First (High Priority - 15 items)
1. Split git-prompt.c into modules
2. Add missing test coverage (submodules, bisect, worktree, etc.)
3. Create TROUBLESHOOTING.md
4. Setup CI/CD pipeline
5. Add static analysis (cppcheck, clang-tidy)
6. Implement configuration file support

### Do Soon (Medium Priority - 19 items)
1. Add error handling and return codes
2. Add dependency tracking to Makefile
3. Create test categories and organization
4. Add performance benchmarking
5. Create ARCHITECTURE.md
6. Enhanced git state detection (bisect, am, progress)
7. Reorganize directory structure
8. Create release automation
9. Add code coverage tooling

### Nice to Have (Low Priority - 20 items)
1. Extract magic numbers to constants
2. Build system improvements
3. Memory allocation optimization
4. Examples gallery
5. Additional indicators (worktree count, etc.)
6. Fuzzing infrastructure

---

## Notes

- This document is a living roadmap and should be updated as items are completed
- Check off items with `[x]` as they're completed
- Add new items as they're discovered
- Review and reprioritize quarterly
