# Release Directory

This directory contains the latest tested and verified version of `git-prompt`.

## How it works

- The binary here is a **hardlink** (not a copy) to `target/git-prompt`
- It is **only updated** when `make release` is run AND all tests pass
- It is **never cleaned** by `make clean` or `make distclean`
- This ensures you always have a known-good binary available

## Usage in your shell

You can safely reference this binary in your shell configuration:

```bash
# In your .bashrc or .bash_profile
if [ -x "$HOME/repo/git-prompt/release/git-prompt" ]; then
    PROMPT_COMMAND='PS1="$($HOME/repo/git-prompt/release/git-prompt)"'
fi
```

Since this directory is never cleaned and only updated with tested binaries,
your prompt will continue working even during development and testing of new
versions.

## Updating the release

To update this binary with a new tested version:

```bash
make release
```

This will:
1. Build all binaries
2. Run the full test suite
3. **Only if tests pass**: hardlink the new binary here
4. **If tests fail**: abort and preserve the existing binary
