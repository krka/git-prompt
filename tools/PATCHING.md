# Source Patching for Size Optimization

## Overview

The experimental `make patched` target applies source patches to Git before building, removing unused backends and functions through strategic dispatch table patching. This enables LTO to eliminate otherwise-reachable code, achieving significant additional size reduction.

## Systematic Patching Strategy

The optimization process follows a systematic approach:

1. **Identify unreachable code** - Build `patched-debug` binary and analyze with `nm` to find functions that remain despite LTO
2. **Find dynamic dispatch tables** - Locate vtables, command tables, and function pointer arrays that prevent LTO from eliminating code
3. **Target high-impact patches** - Focus on dispatch entries that enable elimination of large function families (30+ functions)
4. **Verify safety** - Ensure git-prompt never uses the code paths being removed
5. **Measure impact** - Compare binary size and function count before/after each patch

This iterative process has eliminated **289 KB (31%)** beyond what LTO alone achieves.

## Results

| Build | Size | Reduction from Original | Notes |
|-------|------|------------------------|-------|
| Default | 11 MB | 0% | With debug symbols |
| Optimized | 928 KB | 91.6% | LTO, keeps symbols |
| Stripped | 769 KB | 93.0% | LTO, no symbols |
| **Patched** | **639 KB** | **94.2%** | LTO + source patches |

**Additional savings from patching: 289 KB (31.3% beyond LTO)**

## What Gets Removed

### 1. Reftable Backend (~150 KB, 37 functions)

Git supports two reference storage formats:
- **FILES** - Traditional `.git/refs/` directory structure (what we support)
- **REFTABLE** - New compact binary format (experimental, not needed)

By commenting out the reftable backend from the vtable, LTO can eliminate all 37 reftable functions.

**Patch:**
```c
// In refs.c
static const struct ref_storage_be *refs_backends[] = {
    [REF_STORAGE_FORMAT_FILES] = &refs_be_files,
    // [REF_STORAGE_FORMAT_REFTABLE] = &refs_be_reftable,  // REMOVED
};
```

### 2. FSCK Command (~50 KB, 18 functions)

The `git fsck` command performs repository integrity checks, which are never needed by a read-only prompt tool.

**Patches:**
```c
// In builtin.h - remove declaration
// int cmd_fsck(int argc, const char **argv, const char *prefix, struct repository *repo);

// In git.c - remove command registration
static struct cmd_struct commands[] = {
    ...
    // { "fsck", cmd_fsck, RUN_SETUP },  // REMOVED
};
```

This prevents the entire fsck command and its dependencies from being linked.

### 3. Date Keywords (~2 KB, 8 functions)

Git supports special date keywords like "yesterday", "noon", "tea" (5pm), etc. These are never used when reading repository metadata.

**Patches:**
```c
// In date.c - remove special date keywords from table
static const struct special_day special_days[] = {
    // { "yesterday", date_yesterday },  // REMOVED
    // { "noon", date_noon },  // REMOVED
    // { "midnight", date_midnight },  // REMOVED
    // { "tea", date_tea },  // REMOVED
    // { "PM", date_pm },  // REMOVED
    // { "AM", date_am },  // REMOVED
    // { "never", date_never },  // REMOVED
    // { "now", date_now },  // REMOVED
};
```

### 4. Debug Reference Backend (~12 KB, 58 functions)

Git can wrap the reference backend in a debug tracer for development. This is never needed in production.

**Patch:**
```c
// In refs.c - disable debug wrapping
void repo_set_ref_storage_format(struct repository *r, ...)
{
    ...
    // r->refs_private = maybe_debug_wrap_ref_store(r->gitdir, r->refs_private);  // REMOVED
}
```

This removes the entire debug wrapper vtable and its 58 callback functions.

### 5. Trace2 Telemetry Targets (~45 KB, 76 functions)

Git's trace2 system outputs performance telemetry in three formats (normal, perf, event). Git-prompt never outputs telemetry.

**Patches:**
```c
// In trace2.c - remove all output targets
static struct tr2_tgt *tr2_tgt_builtins[] = {
    // &tr2_tgt_normal,  // REMOVED (27 fn_* callbacks)
    // &tr2_tgt_perf,    // REMOVED (27 fn_* callbacks)
    // &tr2_tgt_event,   // REMOVED (27 fn_* callbacks)
    NULL
};
```

This eliminates all trace2 output formatting code while keeping the lightweight instrumentation hooks.

## What We Kept

### SHA-256 Support

Although SHA-256 repos are rare, they exist and should be supported. The SHA-256 implementation is small and doesn't justify breaking compatibility.

## Safety

The patching system (`tools/apply-patches.py` + `tools/patches.yaml`) has built-in safety features:

1. **Pattern verification** - Fails if expected code patterns aren't found
2. **Automatic restoration** - Git submodule is reset after build via `git-cleanup.sh`
3. **Clean separation** - Patches only applied during patched library builds
4. **Version detection** - Fails explicitly if Git source changes, preventing silent breakage

The build process:
```bash
make all
# 1. Builds raw (unpatched) libraries and caches them
# 2. Cleans Git submodule to pristine state
# 3. Applies patches from patches.yaml
# 4. Builds patched libraries with optimizations
# 5. Cleans Git submodule again (restores original)
# 6. Links final binaries with appropriate libraries
```

## When to Use

- **Production deployments** where every KB matters
- **Embedded systems** with limited storage
- **Container images** to minimize image size
- **Curiosity** about the limits of optimization

## Risks

1. **Git source divergence** - Future Git versions may change vtable structures
2. **Unexpected behavior** - Removing code always carries some risk
3. **Maintenance burden** - Patches need updating when Git version changes

The patching script will fail loudly if Git's source structure changes, preventing silent breakage.

## Remaining Opportunities

Analysis of the patched-debug binary shows **127 unreachable functions** still remain. Most of these are difficult to eliminate because they're referenced through:

1. **Generic utility functions** - Called from many code paths, hard to prove unused
2. **Error handling chains** - Kept alive by error formatting code
3. **Deep library dependencies** - Small gains not worth the complexity

The low-hanging fruit has been picked. Further optimization would require more invasive changes like forking Git's codebase entirely.

## Analysis Tools

Several tools in `tools/` help identify optimization opportunities:

- `analyze-binary.sh` - Identifies unreachable functions using `nm` and call graph analysis
- `apply-patches.py` - Applies patches from `patches.yaml` to Git source
- `git-cleanup.sh` - Resets Git submodule to pristine state after builds
- `patches.yaml` - Declarative patch configuration with safety checks

See `tools/README.md` for detailed usage.

## Conclusion

Through systematic dispatch table patching, we've achieved a **31% additional reduction** beyond LTO optimization, bringing the final binary from 928KB to 639KB. This demonstrates that combining link-time optimization with strategic source patching can eliminate code that would otherwise be kept alive by dynamic dispatch mechanisms.

The approach maintains full compatibility with standard Git repositories (both SHA-1 and SHA-256) while eliminating features that a read-only status tool never uses.
