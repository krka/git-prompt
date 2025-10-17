# Makefile for git-prompt
#
# This tool builds against git as a submodule. The build process is optimized
# to minimize rebuilds - git's object files are only rebuilt when necessary.

# Project settings
PROJECT = git-prompt
SUBMODULE_DIR = submodules/git
SUBMODULE_MARKER = $(SUBMODULE_DIR)/.git

# Build output directory
TARGET_DIR = target

# Release directory (never cleaned, only updated when tests pass)
RELEASE_DIR = release
RELEASE_BINARY = $(RELEASE_DIR)/git-prompt

# Cached Git libraries (built once, reused many times)
GIT_RAW_DIR = $(TARGET_DIR)/git/raw
GIT_PATCHED_DIR = $(TARGET_DIR)/git/patched

GIT_RAW_LIB = $(GIT_RAW_DIR)/libgit.a
GIT_RAW_XDIFF = $(GIT_RAW_DIR)/xdiff-lib.a
GIT_RAW_REFTABLE = $(GIT_RAW_DIR)/reftable-lib.a

GIT_PATCHED_LIB = $(GIT_PATCHED_DIR)/libgit.a
GIT_PATCHED_XDIFF = $(GIT_PATCHED_DIR)/xdiff-lib.a
GIT_PATCHED_REFTABLE = $(GIT_PATCHED_DIR)/reftable-lib.a

# Compiler settings
CC = cc
CFLAGS = -g -O2 -Wall
LDFLAGS =
LIBS = -lpthread -lz -lrt

# Include paths from git's build
GIT_CFLAGS = -I$(SUBMODULE_DIR)

# Main source file
SRC = git-prompt.c

# Core binaries (always built)
EXECUTABLE = $(TARGET_DIR)/git-prompt                          # Patched, stripped, optimized (shipping)
EXECUTABLE_UNPATCHED = $(TARGET_DIR)/git-prompt-unpatched       # Raw baseline (reference)
EXECUTABLE_PATCHED_DEBUG = $(TARGET_DIR)/git-prompt-patched-debug  # Patched with debug (analysis)

# Object files
OBJ_MAIN = $(TARGET_DIR)/git-prompt.o
OBJ_UNPATCHED = $(TARGET_DIR)/git-prompt-unpatched.o
OBJ_PATCHED_DEBUG = $(TARGET_DIR)/git-prompt-patched-debug.o

# Build flags for unpatched (baseline reference)
CFLAGS_UNPATCHED = -g -O2 -Wall
LDFLAGS_UNPATCHED =

# Build flags for main shipping binary (patched, stripped, optimized)
CFLAGS_MAIN = -O2 -Wall -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -flto
LDFLAGS_MAIN = -Wl,--gc-sections -Wl,-O2 -s -flto

# Build flags for patched debug (for dependency analysis)
CFLAGS_PATCHED_DEBUG = -g -O2 -Wall -ffunction-sections -fdata-sections -flto
LDFLAGS_PATCHED_DEBUG = -Wl,--gc-sections -Wl,-O2 -flto

# Sanitizer binaries (for testing)
EXECUTABLE_ASAN = $(TARGET_DIR)/git-prompt-asan
OBJ_ASAN = $(TARGET_DIR)/git-prompt-asan.o
CFLAGS_ASAN = -g -O1 -Wall -fsanitize=address -fno-omit-frame-pointer
LDFLAGS_ASAN = -fsanitize=address

EXECUTABLE_UBSAN = $(TARGET_DIR)/git-prompt-ubsan
OBJ_UBSAN = $(TARGET_DIR)/git-prompt-ubsan.o
CFLAGS_UBSAN = -g -O1 -Wall -fsanitize=undefined -fno-omit-frame-pointer
LDFLAGS_UBSAN = -fsanitize=undefined

# Test configuration
TEST_REPORT = tests/examples.html
TEST_RUNNER = tests/run_tests.py

# Prevent parallel builds of raw and patched libraries
# They both modify the same git submodule directory, so building them in parallel
# causes race conditions. We don't gain much from parallelism anyway since:
# 1. Library builds are expensive but cached (only built once)
# 2. Our own .c compilation is fast (< 1s)
# 3. Git's internal Makefile already parallelizes the submodule build
.NOTPARALLEL: $(GIT_RAW_LIB) $(GIT_PATCHED_LIB)

# Default target - build all binaries (core + sanitizers)
.PHONY: all
all: $(EXECUTABLE) $(EXECUTABLE_UNPATCHED) $(EXECUTABLE_PATCHED_DEBUG) $(EXECUTABLE_ASAN) $(EXECUTABLE_UBSAN)
	@echo ""
	@echo "Build complete! Five binaries built:"
	@ls -lh $(EXECUTABLE_UNPATCHED) $(EXECUTABLE_PATCHED_DEBUG) $(EXECUTABLE) $(EXECUTABLE_ASAN) $(EXECUTABLE_UBSAN) 2>/dev/null | awk '{print $$9 " - " $$5}' || true

# Initialize git submodule if needed
# The .git file/directory is created when the submodule is initialized
$(SUBMODULE_MARKER):
	@echo "Initializing git submodule..."
	@git submodule update --init --recursive

# Update submodule to the commit recorded in the repository
.PHONY: submodule-update
submodule-update:
	@echo "Updating git submodule to recorded commit..."
	@git submodule update --init --recursive

# Clean Git submodule (reset to pristine state)
.PHONY: git-cleanup
git-cleanup:
	@./tools/git-cleanup.sh

# ============================================================================
# Cached Git library builds (build once, reuse many times)
# ============================================================================

# Build raw (unpatched) Git libraries and cache them
$(GIT_RAW_LIB): $(SUBMODULE_MARKER)
	@echo "Building raw Git libraries (unpatched)..."
	@mkdir -p $(GIT_RAW_DIR)
	@./tools/git-cleanup.sh  # Ensure pristine state before building
	@$(MAKE) -C $(SUBMODULE_DIR) CFLAGS="$(CFLAGS_UNPATCHED)" \
		libgit.a xdiff/lib.a reftable/libreftable.a
	@cp $(SUBMODULE_DIR)/libgit.a $(GIT_RAW_LIB)
	@cp $(SUBMODULE_DIR)/xdiff/lib.a $(GIT_RAW_XDIFF)
	@cp $(SUBMODULE_DIR)/reftable/libreftable.a $(GIT_RAW_REFTABLE)
	@echo "✓ Raw libraries cached in $(GIT_RAW_DIR)"

# Other raw libraries depend on the main one
$(GIT_RAW_XDIFF): $(GIT_RAW_LIB)
$(GIT_RAW_REFTABLE): $(GIT_RAW_LIB)

# Build patched Git libraries and cache them
# Depends on patches.yaml and apply-patches.py - auto-rebuild if patches change
# Note: Does NOT depend on raw libraries (no logical dependency)
# Parallelism is prevented by .NOTPARALLEL directive above
$(GIT_PATCHED_LIB): $(SUBMODULE_MARKER) tools/patches.yaml tools/apply-patches.py
	@echo "Building patched Git libraries..."
	@mkdir -p $(GIT_PATCHED_DIR)
	@./tools/git-cleanup.sh  # Ensure pristine state before patching
	@./tools/apply-patches.py
	@$(MAKE) -C $(SUBMODULE_DIR) \
		CC=gcc \
		AR=gcc-ar \
		RANLIB=gcc-ranlib \
		CFLAGS="$(CFLAGS_MAIN)" \
		libgit.a xdiff/lib.a reftable/libreftable.a
	@cp $(SUBMODULE_DIR)/libgit.a $(GIT_PATCHED_LIB)
	@cp $(SUBMODULE_DIR)/xdiff/lib.a $(GIT_PATCHED_XDIFF)
	@cp $(SUBMODULE_DIR)/reftable/libreftable.a $(GIT_PATCHED_REFTABLE)
	@./tools/git-cleanup.sh  # Clean up after building
	@echo "✓ Patched libraries cached in $(GIT_PATCHED_DIR)"

# Other patched libraries depend on the main one
$(GIT_PATCHED_XDIFF): $(GIT_PATCHED_LIB)
$(GIT_PATCHED_REFTABLE): $(GIT_PATCHED_LIB)

# Create target directory if needed
$(TARGET_DIR):
	@mkdir -p $(TARGET_DIR)

# ============================================================================
# Unpatched build (baseline reference)
# ============================================================================
$(OBJ_UNPATCHED): $(SRC) | $(TARGET_DIR)
	$(CC) $(CFLAGS_UNPATCHED) $(GIT_CFLAGS) -c -o $@ $<

$(EXECUTABLE_UNPATCHED): $(OBJ_UNPATCHED) $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE)
	$(CC) $(LDFLAGS_UNPATCHED) -o $@ $< $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE) $(LIBS)

# ============================================================================
# Patched builds (main shipping binary + debug analysis binary)
# ============================================================================
# These builds use cached patched libraries (no need to patch again!)
$(OBJ_MAIN): $(SRC) | $(TARGET_DIR)
	gcc $(CFLAGS_MAIN) $(GIT_CFLAGS) -c -o $@ $<

$(EXECUTABLE): $(OBJ_MAIN) $(GIT_PATCHED_LIB) $(GIT_PATCHED_XDIFF) $(GIT_PATCHED_REFTABLE)
	gcc $(LDFLAGS_MAIN) -o $@ $< $(GIT_PATCHED_LIB) $(GIT_PATCHED_XDIFF) $(GIT_PATCHED_REFTABLE) $(LIBS)

$(OBJ_PATCHED_DEBUG): $(SRC) | $(TARGET_DIR)
	gcc $(CFLAGS_PATCHED_DEBUG) $(GIT_CFLAGS) -c -o $@ $<

$(EXECUTABLE_PATCHED_DEBUG): $(OBJ_PATCHED_DEBUG) $(GIT_PATCHED_LIB) $(GIT_PATCHED_XDIFF) $(GIT_PATCHED_REFTABLE)
	gcc $(LDFLAGS_PATCHED_DEBUG) -o $@ $< $(GIT_PATCHED_LIB) $(GIT_PATCHED_XDIFF) $(GIT_PATCHED_REFTABLE) $(LIBS)

# Test all binaries (test runner auto-discovers and tests all available binaries)
.PHONY: test
test: all
	@cd tests && ./run_tests.py

# Update test expectations (run tests with --replace-expected)
.PHONY: test-update
test-update: all
	@cd tests && ./run_tests.py --replace-expected

# Generate HTML documentation with examples
.PHONY: docs
docs: test
	@echo "Generating documentation..."
	@cd tests && ./generate_docs.py
	@echo "✓ Documentation ready: tests/documentation.html"

# Publish documentation to GitHub Pages using worktree
.PHONY: gh-pages
gh-pages: docs
	@echo "Publishing documentation to gh-pages branch..."
	@if [ ! -d gh-pages ]; then \
		if git show-ref --quiet refs/heads/gh-pages; then \
			echo "Adding worktree for existing gh-pages branch..."; \
			git worktree add gh-pages gh-pages; \
		else \
			echo "Creating orphan gh-pages worktree (first time setup)..."; \
			git worktree add --orphan gh-pages; \
			cd gh-pages && git reset; \
		fi; \
	fi
	@cp tests/documentation.html gh-pages/index.html
	@cp tests/test-styles.css gh-pages/test-styles.css
	@cd gh-pages && \
		git add index.html test-styles.css && \
		(git commit -m "Update documentation" || echo "No changes to commit") && \
		git push --force-with-lease -u origin gh-pages
	@echo ""
	@echo "✓ Documentation published to GitHub Pages!"
	@echo "  Worktree location: gh-pages/"
	@echo "  Configure GitHub repo settings to use gh-pages branch"
	@echo "  Settings → Pages → Source: gh-pages branch"

# ============================================================================
# Static Analysis Targets
# ============================================================================

# Run static analysis with cppcheck and clang-tidy
.PHONY: analyze
analyze:
	@echo "Running static analysis..."
	@which cppcheck > /dev/null || { echo "Error: cppcheck not found. Install with: sudo apt-get install cppcheck"; exit 1; }
	@which clang-tidy > /dev/null || { echo "Error: clang-tidy not found. Install with: sudo apt-get install clang-tidy"; exit 1; }
	@echo ""
	@echo "=== cppcheck ==="
	@cppcheck --enable=all --suppress=missingIncludeSystem \
		--suppress=toomanyconfigs \
		--inline-suppr --std=c99 \
		-I$(SUBMODULE_DIR) -i$(SUBMODULE_DIR) \
		--suppress='*:$(SUBMODULE_DIR)/*' \
		$(SRC) 2>&1 | grep -v "Checking $(SRC)"
	@echo ""
	@echo "=== clang-tidy ==="
	@clang-tidy $(SRC) -- $(GIT_CFLAGS)

# ============================================================================
# Sanitizer builds (for memory error detection and undefined behavior)
# ============================================================================
$(OBJ_ASAN): $(SRC) | $(TARGET_DIR)
	$(CC) $(CFLAGS_ASAN) $(GIT_CFLAGS) -c -o $@ $<

$(EXECUTABLE_ASAN): $(OBJ_ASAN) $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE)
	$(CC) $(LDFLAGS_ASAN) -o $@ $< $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE) $(LIBS)

$(OBJ_UBSAN): $(SRC) | $(TARGET_DIR)
	$(CC) $(CFLAGS_UBSAN) $(GIT_CFLAGS) -c -o $@ $<

$(EXECUTABLE_UBSAN): $(OBJ_UBSAN) $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE)
	$(CC) $(LDFLAGS_UBSAN) -o $@ $< $(GIT_RAW_LIB) $(GIT_RAW_XDIFF) $(GIT_RAW_REFTABLE) $(LIBS)

# Run all static analysis tools (cppcheck and clang-tidy)
.PHONY: check-all
check-all: analyze
	@echo ""
	@echo "✓ All static analysis checks complete"

# Release target: build, test, and hardlink to release/ if tests pass
.PHONY: release
release: all
	@echo "Running tests before release..."
	@if $(MAKE) test; then \
		mkdir -p $(RELEASE_DIR); \
		rm -f $(RELEASE_BINARY); \
		ln $(EXECUTABLE) $(RELEASE_BINARY); \
		echo ""; \
		echo "✓ Tests passed! Binary released to $(RELEASE_BINARY)"; \
		ls -lh $(RELEASE_BINARY) | awk '{print "  " $$9 " - " $$5}'; \
	else \
		echo ""; \
		echo "✗ Tests failed! Release aborted."; \
		exit 1; \
	fi

# Full release: run tests + static analysis before releasing
# This is slower but catches more issues. Use for major releases.
# Note: `make test` automatically tests all 5 binaries (including asan and ubsan)
.PHONY: release-full
release-full: all
	@echo "Running tests and static analysis before release..."
	@if $(MAKE) test && $(MAKE) analyze; then \
		mkdir -p $(RELEASE_DIR); \
		rm -f $(RELEASE_BINARY); \
		ln $(EXECUTABLE) $(RELEASE_BINARY); \
		echo ""; \
		echo "✓ Tests and analysis passed! Binary released to $(RELEASE_BINARY)"; \
		ls -lh $(RELEASE_BINARY) | awk '{print "  " $$9 " - " $$5}'; \
	else \
		echo ""; \
		echo "✗ Tests or analysis failed! Release aborted."; \
		exit 1; \
	fi

# Quick check: fast static analysis only (no execution)
# Use this during development - runs in seconds
.PHONY: quick-check
quick-check:
	@echo "Running quick static analysis (no execution)..."
	@$(MAKE) analyze

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(TARGET_DIR)
	rm -f $(GIT_LIB_OPT) $(GIT_XDIFF_LIB_OPT) $(GIT_REFTABLE_LIB_OPT)
	rm -f $(TEST_REPORT) tests/test-results.html tests/test-results.txt
	rm -f tests/examples-doc.html tests/examples-snippet.html tests/documentation.html

# Deep clean including git submodule build
.PHONY: distclean
distclean: clean
	@if [ -d $(SUBMODULE_DIR) ]; then \
		echo "Cleaning git submodule build..."; \
		$(MAKE) -C $(SUBMODULE_DIR) clean; \
	fi

# Install to /usr/local/bin (or PREFIX if set)
PREFIX ?= /usr/local
.PHONY: install
install: $(EXECUTABLE)
	install -d $(PREFIX)/bin
	install -m 755 $(EXECUTABLE) $(PREFIX)/bin/

# Install to user's local bin directory (no sudo required)
.PHONY: install-local
install-local: $(EXECUTABLE)
	install -d $(HOME)/.local/bin
	install -m 755 $(EXECUTABLE) $(HOME)/.local/bin/
	@echo "✓ Installed to $(HOME)/.local/bin/git-prompt"
	@echo "  Make sure $(HOME)/.local/bin is in your PATH"

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(EXECUTABLE)

# Help target
.PHONY: help
help:
	@echo "git-prompt build targets:"
	@echo "  all              - Build all binaries (default: 3 core + 2 sanitizers)"
	@echo "  test             - Run tests (auto-discovers and tests all binaries)"
	@echo "  test-update      - Run tests and update expectations (--replace-expected)"
	@echo "  docs             - Generate HTML documentation with examples"
	@echo "  gh-pages         - Publish documentation to GitHub Pages (uses worktree)"
	@echo "  release          - Build, test, and hardlink to release/ (fast, tests only)"
	@echo "  release-full     - Build, test, analyze, and release (slower, recommended for major releases)"
	@echo "  clean            - Remove build artifacts (preserves release/)"
	@echo "  distclean        - Remove all build artifacts including git submodule"
	@echo "  submodule-update - Update git submodule to recorded commit"
	@echo "  install          - Install shipping binary to $(PREFIX)/bin"
	@echo "  install-local    - Install shipping binary to ~/.local/bin (no sudo)"
	@echo "  uninstall        - Remove from $(PREFIX)/bin"
	@echo ""
	@echo "Static analysis targets:"
	@echo "  quick-check      - Fast: cppcheck + clang-tidy (seconds, no execution)"
	@echo "  analyze          - Fast: same as quick-check"
	@echo "  check-all        - Run static analysis tools"
	@echo ""
	@echo "Five binaries built by default:"
	@echo "  git-prompt               - Patched, stripped, optimized (shipping, ~688 KB)"
	@echo "  git-prompt-unpatched     - Raw baseline reference (~11 MB)"
	@echo "  git-prompt-patched-debug - Patched with debug symbols for analysis (~11 MB)"
	@echo "  git-prompt-asan          - Address Sanitizer build (~11 MB)"
	@echo "  git-prompt-ubsan         - Undefined Behavior Sanitizer build (~11 MB)"
	@echo ""
	@echo "The unpatched binary is the source of truth - if test outputs differ,"
	@echo "the unpatched version is correct and patched versions are broken."
	@echo ""
	@echo "First build will compile git libraries (~30s), subsequent builds are fast."
