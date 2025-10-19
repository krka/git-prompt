/*
 * git-prompt - Fast git repository status for shell prompts
 *
 * A standalone tool that displays colorful git repository status optimized
 * for shell prompt integration.
 */
#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "config.h"
#include "environment.h"
#include "path.h"
#include "refs.h"
#include "object-name.h"
#include "object-file.h"
#include "commit.h"
#include "commit-reach.h"
#include "diff.h"
#include "revision.h"
#include "run-command.h"
#include "setup.h"
#include "strbuf.h"
#include "parse-options.h"
#include "read-cache.h"
#include "cache-tree.h"
#include "wt-status.h"
#include "dir.h"
#include "oidset.h"
#include "oidmap.h"
#include "remote.h"
#include "hex.h"
#include "graph-distance.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>

/*
 * ANSI color codes for terminal output
 */
#define COLOR_CLEAN "32"      /* Green - clean repo */
#define COLOR_UNTRACKED "36"  /* Cyan - untracked files only (informational) */
#define COLOR_UNSTAGED "33"   /* Yellow - tracked but unstaged changes (warning) */
#define COLOR_STAGED "33"     /* Yellow - staged changes (need to commit) */
#define COLOR_MODIFIED "31"   /* Red - unstaged modifications to tracked files */
#define COLOR_LARGE_REPO "37" /* White/gray - large repo (status unknown) */
#define COLOR_PURPLE "35"
#define COLOR_CYAN "36"

/* Indicator colors */
#define COLOR_AHEAD "34"    /* Blue - ahead (should push) */
#define COLOR_BEHIND "33"   /* Yellow - behind */
#define COLOR_DETACHED "35" /* Magenta - detached HEAD */
#define COLOR_MERGE "36"    /* Cyan - merge/rebase in progress */
#define COLOR_DIVERGED "31" /* Red - diverged */
#define COLOR_STASH "36"    /* Cyan - stashed changes */
#define COLOR_CONFLICT "31" /* Red - conflicts */

/* Performance threshold */
#define LARGE_REPO_INDEX_SIZE 5000000 /* 5MB */
#define MAX_TRAVERSAL_DEFAULT                                                                      \
  1000                      /* Default traversal limit per phase (balances accuracy vs speed) */
#define BFS_QUEUE_SIZE 2048 /* Power of 2 for fast modulo via bitwise AND */

/*
 * PERFORMANCE ANALYSIS: Function Complexity and Large Repo Mode Safety
 * ====================================================================
 *
 * Large repo mode (triggered when .git/index > LARGE_REPO_INDEX_SIZE) uses GRAY
 * branch color as a performance fallback. The goal is to skip expensive operations
 * while still showing useful information.
 *
 * SAFE FOR LARGE REPO MODE (can call without performance penalty):
 * ----------------------------------------------------------------
 * - is_large_repo()              O(1)      - Single stat() syscall
 * - get_git_state()              O(1)*     - File existence checks (*O(n) if checking conflicts)
 * - check_git_state_file()       O(1)*     - File access() syscall (*O(n) if checking conflicts)
 * - get_misc_indicators()        O(1)      - Flag checks and ref existence
 * - get_tracking_indicators()    O(commits)- Graph traversal (limited by max_traversal)
 * - bfs_find_distance()          O(commits)- BFS limited by max_traversal, with internal caching
 *
 * UNSAFE FOR LARGE REPO MODE (expensive, currently skipped):
 * ----------------------------------------------------------
 * - has_unmerged_files()         O(n)      - Scans all index entries
 * - has_staged_changes()         O(n)      - Scans index, rebuilds cache-tree
 * - has_worktree_changes()       O(n+m)    - Stats all tracked files in worktree
 * - get_branch_name_and_color()  O(n+m)*   - Calls has_worktree_changes() and has_staged_changes()
 *                                           (*O(1) for branch name, expensive for color)
 *
 * CURRENT LARGE REPO MODE BEHAVIOR:
 * - Branch color: GRAY (skip status checks) UNLESS conflicts detected (then RED)
 * - Branch name: Still computed (cheap ref operations)
 * - Tracking indicators: Still computed (graph operations, bounded by max_traversal)
 * - Misc indicators: Still computed (cheap file checks)
 * - Git state detection: FULLY computed - index loaded conditionally when git operations detected
 *   - State file check is O(1) (fast)
 *   - Index loaded ONLY when merge/rebase/cherry-pick/revert detected
 *   - Conflict detection is O(n) but critical, so worth the cost during active operations
 *
 * RATIONALE FOR CONFLICT DETECTION IN LARGE REPOS:
 * - Conflicts are CRITICAL information that must always be accurate
 * - Checking for state files is O(1) (5 fast access() calls)
 * - Loading index is O(n) but only happens during active git operations
 * - During merge/rebase, users NEED to see conflict status immediately
 * - Normal large repo usage (no active operations) remains fast
 */

/* Global flags */
static int use_color = 1;
static int debug_mode = 0;
static long large_repo_size = LARGE_REPO_INDEX_SIZE;
static int local_mode = 0;
static int max_traversal = MAX_TRAVERSAL_DEFAULT;

/* Debug timing macros */
#define DEBUG_TIMER_START(name)                                                                    \
  struct timeval tv_start_##name, tv_end_##name;                                                   \
  if (debug_mode)                                                                                  \
    gettimeofday(&tv_start_##name, NULL);

#define DEBUG_TIMER_END(name, label)                                                               \
  if (debug_mode) {                                                                                \
    gettimeofday(&tv_end_##name, NULL);                                                            \
    long usec = (tv_end_##name.tv_sec - tv_start_##name.tv_sec) * 1000000 +                        \
                (tv_end_##name.tv_usec - tv_start_##name.tv_usec);                                 \
    fprintf(stderr, "[DEBUG] %s: %.3fms\n", label, usec / 1000.0);                                 \
  }

static const char *const prompt_usage[] = {
  "git prompt [--help] [--no-color] [--debug] [--large-repo-size=<bytes>] "
  "[--max-traversal=<commits>] [--local]",
  "git prompt distance --from=<commit> --to=<commit> [--max-traversal=<commits>] [--debug]", NULL};

static const char prompt_help[] =
  "git prompt - Display colorful git repository status for shell prompts\n"
  "\n"
  "OUTPUT FORMAT:\n"
  "  [branch] indicators\n"
  "\n"
  "BRANCH COLORS:\n"
  "  Green   - Clean working tree (no changes, nothing staged)\n"
  "  Yellow  - Staged changes (ready to commit)\n"
  "  Red     - Unstaged changes or conflicts (need attention)\n"
  "  Cyan    - Untracked files only (informational)\n"
  "  Gray    - Large repository (status check skipped for performance)\n"
  "\n"
  "INDICATORS:\n"
  "  ⚡        - Detached HEAD\n"
  "  [state]  - Git operation in progress (merge, rebase, cherry-pick, revert)\n"
  "             Red if conflicts present, cyan otherwise\n"
  "\n"
  "UPSTREAM TRACKING (shown in parentheses for branches with configured upstream):\n"
  "  (↑N)     - N commits ahead of upstream (blue - ready to push)\n"
  "  (↓N)     - N commits behind upstream (yellow - need to pull)\n"
  "  (↑N↓M)   - N commits ahead, M commits behind (diverged, red)\n"
  "  (↕)      - Too far diverged (>max-traversal commits, red)\n"
  "  (nothing shown when in sync with upstream)\n"
  "\n"
  "OTHER INDICATORS:\n"
  "  ○        - No upstream configured (magenta)\n"
  "\n"
  "DISTANCE FROM MAIN (shown for feature branches):\n"
  "  ↑N       - N commits ahead of origin/main or origin/master (blue)\n"
  "  ↓N       - N commits behind origin/main or origin/master (yellow)\n"
  "  ↑N↓M     - N commits ahead, M commits behind\n"
  "  ↕        - Too far diverged from main (>max-traversal commits, red)\n"
  "\n"
  "OTHER INDICATORS:\n"
  "  💾       - Stashed changes present (cyan)\n"
  "\n"
  "EXAMPLES:\n"
  "  [main]                - On main, in sync with upstream, clean\n"
  "  [feature] ○           - On feature, no upstream, clean\n"
  "  [main] (↑2)           - On main, 2 commits ahead of upstream, clean\n"
  "  [feature] ↑5↓3        - On feature, 5 ahead/3 behind main, synced with upstream\n"
  "  [feature] ↑10(↑2)     - Feature: 10 ahead of main, 2 unpushed to upstream\n"
  "  [main] ⚡ [merge:conflict]  - Detached HEAD, merge with conflicts\n"
  "  [feature] 💾          - On feature, has stashed changes\n"
  "\n"
  "PERFORMANCE:\n"
  "  For large repositories (>5MB index), status checks are skipped for speed.\n"
  "  Distance calculation is limited to 1000 commits by default (configurable with "
  "--max-traversal).\n"
  "  Results are cached in .git/distance-cache/ when BFS visits >=10 commits.\n"
  "  Cache maintains up to 100 files (LRU eviction), one file per commit pair.\n"
  "\n"
  "  NOTE: Ahead/behind counts use shortest path in graph, not git's default calculation.\n"
  "  This measures minimum commits between branches, which may differ from git commands.\n"
  "\n"
  "SHELL INTEGRATION:\n"
  "  Bash:  PS1='$(git prompt)\\$ '\n"
  "  Zsh:   setopt PROMPT_SUBST; PROMPT='$(git prompt)%% '\n"
  "  Fish:  function fish_prompt; git prompt; end\n"
  "\n"
  "DISTANCE SUBCOMMAND:\n"
  "  git prompt distance --from=<commit> --to=<commit>\n"
  "\n"
  "  Compute graph distance between two commits. Useful for scripts and tools.\n"
  "  Output format: \"ahead,behind\" (e.g., \"12,3\" or \"-1,-1\" if diverged)\n"
  "\n"
  "  Examples:\n"
  "    git prompt distance --from=HEAD --to=origin/main\n"
  "    git prompt distance --from=feature --to=main\n";

static void show_help(void)
{
  puts(prompt_help);
}

__attribute__((format(printf, 2, 3))) static void color_printf(const char *color, const char *fmt,
                                                               ...)
{
  va_list ap;
  if (use_color) {
    printf("\001\033[01;%sm\002", color);
  }
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if (use_color) {
    printf("\001\033[00m\002");
  }
}

__attribute__((format(printf, 3, 4))) static void
strbuf_color_addf(struct strbuf *sb, const char *color, const char *fmt, ...)
{
  va_list ap;
  if (use_color) {
    strbuf_addf(sb, "\001\033[01;%sm\002", color);
  }
  va_start(ap, fmt);
  strbuf_vaddf(sb, fmt, ap);
  va_end(ap);
  if (use_color) {
    strbuf_addstr(sb, "\001\033[00m\002");
  }
}

/*
 * Check if repository is large based on index file size.
 * Performance: O(1) - single stat() syscall
 * Safe for large repo mode: Yes (this determines large repo mode)
 */
static int is_large_repo(void)
{
  struct stat st;
  struct strbuf index_file = STRBUF_INIT;

  strbuf_addf(&index_file, "%s/index", repo_get_git_dir(the_repository));

  if (!stat(index_file.buf, &st) && st.st_size > large_repo_size) {
    strbuf_release(&index_file);
    return 1;
  }

  strbuf_release(&index_file);
  return 0;
}


/*
 * Git state information (merge, rebase, cherry-pick, etc.)
 * This is computed once and used both for branch color determination
 * and for displaying the state indicator in the prompt.
 */
struct git_state {
  int has_state;           /* 1 if any git operation is in progress */
  int has_conflicts;       /* 1 if unmerged files exist */
  const char *state_name;  /* e.g., "merge:conflict", "rebase:continue" */
  const char *state_color; /* Color for the state indicator */
};

/*
 * Check if the index (already loaded) has unmerged files.
 * Caller must ensure repo_read_index() was called successfully.
 * Performance: O(n) where n = number of index entries (worst case: full scan)
 *              In practice, returns early on first unmerged entry
 * Safe for large repo mode: No (requires loaded index, scans all entries)
 */
static int has_unmerged_files(void)
{
  int i;

  for (i = 0; i < the_repository->index->cache_nr; i++) {
    const struct cache_entry *ce = the_repository->index->cache[i];
    if (ce_stage(ce)) {
      return 1;
    }
  }

  return 0;
}

/*
 * Check if there are staged changes (index differs from HEAD).
 *
 * Uses cache-tree to efficiently detect if the index matches HEAD's tree.
 * If cache-tree is invalid, we rebuild it to get an accurate comparison.
 *
 * Special case: During conflicts (merge/rebase/cherry-pick), unmerged entries
 * count as staged changes even if cache-tree matches HEAD.
 *
 * Performance: O(n) where n = number of index entries
 *              cache_tree_update() rebuilds cache-tree by scanning index
 * Safe for large repo mode: No (requires loaded index, potentially expensive)
 *
 * Returns 1 if there are staged changes, 0 otherwise.
 */
static int has_staged_changes(struct repository *r, const struct object_id *head_oid,
                              const struct git_state *state)
{
  struct index_state *istate;

  /* Ensure index is loaded */
  if (repo_read_index(r) < 0) {
    return 0;
  }

  istate = r->index;

  /*
	 * During conflicts, unmerged entries are considered staged changes.
	 * Check this FIRST before doing cache-tree comparison, because
	 * cache_tree_update() can succeed and match HEAD even when unmerged entries exist.
	 */
  if (state->has_conflicts) {
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] has_staged_changes = 1 (conflicts present)\n");
    }
    return 1;
  }

  /* Get HEAD's tree for comparison */
  struct commit *head_commit = lookup_commit(r, head_oid);
  if (!head_commit || repo_parse_commit(r, head_commit)) {
    /* Can't parse HEAD, conservatively report no changes */
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] has_staged_changes = 0 (can't parse HEAD)\n");
    }
    return 0;
  }

  struct tree *head_tree = repo_get_commit_tree(r, head_commit);
  if (!head_tree) {
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] has_staged_changes = 0 (can't get HEAD tree)\n");
    }
    return 0;
  }

  /*
	 * Ensure cache-tree is valid by updating it if needed.
	 * cache_tree_update() rebuilds the cache-tree from the index.
	 * If it succeeds, cache_tree->oid represents the tree the index would create.
	 */
  if (!istate->cache_tree) {
    istate->cache_tree = cache_tree();
  }

  if (cache_tree_update(istate, 0) < 0) {
    /* Cache-tree update failed, fall back to conservative answer */
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] has_staged_changes = 0 (cache-tree update failed)\n");
    }
    return 0;
  }

  /* Compare cache-tree OID with HEAD tree OID */
  int has_changes = !oideq(&istate->cache_tree->oid, &head_tree->object.oid);

  if (debug_mode) {
    fprintf(stderr, "[DEBUG] has_staged_changes = %d (cache-tree OID %s HEAD tree)\n", has_changes,
            has_changes ? "!=" : "==");
  }

  return has_changes;
}

/*
 * Check if there are unstaged changes in the working tree.
 *
 * Uses refresh_index() to update stat info and check if any tracked files
 * have been modified but not staged.
 *
 * Performance: O(n + m) where n = index entries, m = worktree files
 *              refresh_index() stats all tracked files in the working tree
 * Safe for large repo mode: No (requires loaded index, expensive filesystem operations)
 *
 * Returns 1 if any tracked files have unstaged changes, 0 otherwise.
 */
static int has_worktree_changes(struct repository *r)
{
  struct index_state *istate;
  int changed = 0;

  if (repo_read_index(r) < 0) {
    return 0; /* treat unreadable index as clean */
  }

  istate = r->index;

  /* Refresh index to update stat info (REFRESH_UNMERGED suppresses "needs merge" warnings) */
  refresh_index(istate, REFRESH_QUIET | REFRESH_UNMERGED, NULL, NULL, NULL);

  /* Check if any files have changes by looking at cache entries */
  for (int i = 0; i < istate->cache_nr; i++) {
    const struct cache_entry *ce = istate->cache[i];
    if (ce_stage(ce)) {
      continue; /* Skip unmerged entries */
    }

    /* Skip submodule entries - they're handled specially by git status */
    if (S_ISGITLINK(ce->ce_mode)) {
      continue;
    }

    if (!ce_uptodate(ce)) {
      changed = 1;
      if (debug_mode) {
        fprintf(stderr,
                "[DEBUG] File not up-to-date: %s (flags=0x%x, "
                "stat_valid=%d)\n",
                ce->name, ce->ce_flags, (ce->ce_flags & CE_VALID) != 0);
      }
      break;
    }
  }

  return changed;
}

/*
 * Helper to check for a git state file and populate git_state if found.
 * Performance: O(1) - single access() syscall to check file existence
 *              If file exists and index loaded, calls has_unmerged_files() which is O(n)
 * Safe for large repo mode: Partially (file check is O(1), but conflict detection requires index)
 *
 * Returns 1 if the state file exists, 0 otherwise.
 */
static int check_git_state_file(const char *gitdir, const char *filename, struct git_state *state,
                                int index_loaded, const char *state_conflict,
                                const char *state_normal)
{
  struct strbuf path = STRBUF_INIT;
  int found = 0;

  strbuf_addf(&path, "%s/%s", gitdir, filename);

  if (!access(path.buf, F_OK)) {
    int has_conflicts = 0;
    if (index_loaded) {
      has_conflicts = has_unmerged_files();
    }

    state->has_state = 1;
    state->has_conflicts = has_conflicts;
    state->state_name = has_conflicts ? state_conflict : state_normal;
    state->state_color = has_conflicts ? COLOR_CONFLICT : COLOR_MERGE;
    found = 1;
  }

  strbuf_release(&path);
  return found;
}

/*
 * Quick check if any git state files exist (merge, rebase, cherry-pick, revert).
 * This is O(1) - just checks for file existence without loading index.
 * Used to decide if we need to load index in large repo mode for conflict detection.
 *
 * Performance: O(1) - checks 5 files via access() syscalls
 * Safe for large repo mode: Yes (no index operations)
 *
 * Returns 1 if any state file exists, 0 otherwise.
 */
static int has_git_state_files(void)
{
  const char *gitdir = repo_get_git_dir(the_repository);
  struct strbuf path = STRBUF_INIT;
  int found = 0;
  const char *state_files[] = {"rebase-merge",     "rebase-apply", "MERGE_HEAD",
                               "CHERRY_PICK_HEAD", "REVERT_HEAD",  NULL};

  for (int i = 0; state_files[i]; i++) {
    strbuf_reset(&path);
    strbuf_addf(&path, "%s/%s", gitdir, state_files[i]);
    if (!access(path.buf, F_OK)) {
      found = 1;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] Found git state file: %s\n", state_files[i]);
      }
      break;
    }
  }

  strbuf_release(&path);
  return found;
}

/*
 * Detect special git states (merge, rebase, etc.) and populate git_state struct.
 * Requires index_loaded to be 1 for conflict detection.
 *
 * Performance: O(1) for state file checks, O(n) if conflicts need detection
 *              Checks 5 state files via access() syscalls (fast)
 *              If state exists and index loaded, calls has_unmerged_files() O(n)
 * Safe for large repo mode: Partially (state detection is O(1), conflict detection requires index)
 *
 * Returns a git_state struct with all relevant information.
 */
static struct git_state get_git_state(int index_loaded)
{
  struct git_state state = {0, 0, NULL, NULL};
  const char *gitdir = repo_get_git_dir(the_repository);

  /* Check for rebase (interactive or apply mode) */
  if (check_git_state_file(gitdir, "rebase-merge", &state, index_loaded, "rebase:conflict",
                           "rebase:continue")) {
    return state;
  }

  if (check_git_state_file(gitdir, "rebase-apply", &state, index_loaded, "rebase:conflict",
                           "rebase:continue")) {
    return state;
  }

  /* Check for merge */
  if (check_git_state_file(gitdir, "MERGE_HEAD", &state, index_loaded, "merge:conflict",
                           "merge:commit")) {
    return state;
  }

  /* Check for cherry-pick */
  if (check_git_state_file(gitdir, "CHERRY_PICK_HEAD", &state, index_loaded, "cherrypick:conflict",
                           "cherrypick:commit")) {
    return state;
  }

  /* Check for revert */
  check_git_state_file(gitdir, "REVERT_HEAD", &state, index_loaded, "revert:conflict",
                       "revert:commit");

  return state;
}

/*
 * Shared context for prompt generation.
 * Filled once at startup and passed to all helper functions.
 */
struct prompt_context {
  struct object_id oid;   /* HEAD commit */
  struct ref_store *refs; /* Ref store */
  int large_repo;         /* Large repo flag */
  int index_loaded;       /* Index loaded flag */
};

/*
 * Section 1: Determine branch name and color based on working tree state.
 * This is pure filesystem operations - no network refs needed.
 *
 * Takes git_state to check for conflicts when determining staged changes.
 *
 * Performance: Large repo mode: O(1) - only ref resolution and tag lookup
 *              Small repo mode: O(n + m) - calls has_worktree_changes() and has_staged_changes()
 *                n = index entries, m = worktree files
 *              Untracked check uses fill_directory() which is expensive
 * Safe for large repo mode: Partially (branch name is fast, color determination skipped)
 *
 * Returns:
 *   detached - 1 if in detached HEAD state, 0 if on a branch
 */
static int get_branch_name_and_color(struct strbuf *branch, const char **color,
                                     const struct prompt_context *ctx,
                                     const struct git_state *state)
{
  const char *branch_name;
  int detached = 0;

  DEBUG_TIMER_START(branch_name);

  /* Get current branch or detached HEAD */
  branch_name = refs_resolve_ref_unsafe(ctx->refs, "HEAD", 0, NULL, NULL);
  if (branch_name && skip_prefix(branch_name, "refs/heads/", &branch_name)) {
    strbuf_addstr(branch, branch_name);
  } else {
    /* Detached HEAD */
    detached = 1;

    /* Try to get a tag name (skip for large repos) */
    if (!ctx->large_repo) {
      struct commit *commit = lookup_commit_reference(the_repository, &ctx->oid);
      if (commit) {
        const struct name_decoration *decoration = get_name_decoration(&commit->object);
        /* Iterate through decorations to find a tag */
        while (decoration) {
          if (decoration->type == DECORATION_REF_TAG) {
            const char *tag_name = decoration->name;
            /* Strip refs/tags/ prefix if present */
            skip_prefix(tag_name, "refs/tags/", &tag_name);
            strbuf_addstr(branch, tag_name);
            break;
          }
          decoration = decoration->next;
        }
      }
    }

    /* Fallback to short commit hash */
    if (!branch->len) {
      strbuf_add_unique_abbrev(branch, &ctx->oid, 7);
    }
  }

  DEBUG_TIMER_END(branch_name, "Branch name");

  /* Determine color based on working tree and staging area state */
  /* Check conflicts FIRST - they always take priority regardless of repo size */
  if (ctx->index_loaded && state->has_conflicts) {
    /* Conflicts always show RED - need immediate attention */
    *color = COLOR_MODIFIED;
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] Color: RED (conflicts)\n");
    }
  } else if (state->has_state) {
    /* Git operation in progress (merge/rebase/cherry-pick) - staged changes exist */
    *color = COLOR_STAGED;
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] Color: YELLOW (git operation in progress: %s)\n", state->state_name);
    }
  } else if (ctx->large_repo) {
    /* Large repo mode - skip expensive status checks, show GRAY as fallback */
    *color = COLOR_LARGE_REPO;
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] Color: GRAY (large repo mode)\n");
    }
  } else if (!ctx->index_loaded) {
    /* Can't read index, treat as clean */
    *color = COLOR_CLEAN;
  } else {
    DEBUG_TIMER_START(status_check);

    /* Check for unstaged changes (working tree differs from index) */
    int unstaged = has_worktree_changes(the_repository);

    /* Check for staged changes (index differs from HEAD) */
    int staged = has_staged_changes(the_repository, &ctx->oid, state);

    DEBUG_TIMER_END(status_check, "Status: change check");

    if (debug_mode) {
      fprintf(stderr, "[DEBUG] has_worktree_changes = %d\n", unstaged);
      fprintf(stderr, "[DEBUG] has_staged_changes = %d\n", staged);
    }

    if (unstaged) {
      /* Unstaged changes take priority - RED (action needed before staging) */
      *color = COLOR_MODIFIED;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] Color: RED (unstaged changes)\n");
      }
    } else if (staged) {
      /* Staged changes - YELLOW (ready to commit) */
      *color = COLOR_STAGED;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] Color: YELLOW (staged changes)\n");
      }
    } else {
      /* No tracked changes - check for untracked files */
      DEBUG_TIMER_START(status_untracked);
      struct dir_struct dir = DIR_INIT;
      struct pathspec pathspec;

      memset(&pathspec, 0, sizeof(pathspec));
      dir.flags = DIR_SHOW_OTHER_DIRECTORIES | DIR_HIDE_EMPTY_DIRECTORIES;
      setup_standard_excludes(&dir);

      fill_directory(&dir, the_repository->index, &pathspec);

      if (debug_mode && dir.nr > 0) {
        fprintf(stderr, "[DEBUG] Found %d untracked entries, first: %s\n", dir.nr,
                dir.entries[0]->name);
      }

      if (dir.nr > 0) {
        /* Untracked files only (cyan - informational) */
        *color = COLOR_UNTRACKED;
        if (debug_mode) {
          fprintf(stderr, "[DEBUG] Color: CYAN (untracked files)\n");
        }
      } else {
        /* Clean working tree (green - ideal) */
        *color = COLOR_CLEAN;
        if (debug_mode) {
          fprintf(stderr, "[DEBUG] Color: GREEN (clean)\n");
        }
      }

      dir_clear(&dir);
      DEBUG_TIMER_END(status_untracked, "Status: untracked check");
    }
  }

  return detached;
}

/*
 * Section 2: Collect tracking indicators using BFS.
 * Uses bidirectional BFS to determine ahead/behind/diverged status.
 *
 * Two-phase approach:
 * Phase 1: Check distance from origin/master (main codebase)
 * Phase 2: Check distance from upstream tracking branch (what you pushed)
 *
 * Performance: O(commits) where commits ≤ 2 * max_traversal (default 2000)
 *              BFS results are automatically cached internally by graph-distance module
 *              Fast path: cache hit is O(1) (file read + parse)
 *              Slow path: BFS traversal limited by max_traversal
 * Safe for large repo mode: Yes (graph operations, independent of worktree/index)
 */
static void get_tracking_indicators(struct strbuf *indicators, int detached,
                                    const struct strbuf *branch, const struct prompt_context *ctx)
{
  /* Fast exit: detached HEAD has no tracking */
  if (detached) {
    return;
  }

  /*
	 * Phase 1: Check distance from default remote's default branch
	 * Skip if we're on the main branch itself.
	 */
  DEBUG_TIMER_START(distance);

  const char *main_branch = NULL;
  char *main_branch_allocated = NULL; /* Track if main_branch was allocated */
  int main_from_symref = 0; /* 1 if main_branch from origin/HEAD symref, 0 if from fallback */

  /* Determine the default remote name (e.g., "origin", "upstream", etc.) */
  const char *remote_name = NULL;
  struct branch *current_branch = branch_get(NULL);
  if (current_branch) {
    /* Get the remote for this branch (handles branch.<name>.remote config) */
    remote_name = remote_for_branch(current_branch, NULL);
  }

  /* Fallback to "origin" if no remote is configured */
  if (!remote_name) {
    remote_name = "origin";
  }

  if (debug_mode) {
    fprintf(stderr, "[DEBUG] Using remote: %s\n", remote_name);
  }

  /* Try to detect remote's default branch via <remote>/HEAD symbolic ref */
  char remote_head_ref[256];
  snprintf(remote_head_ref, sizeof(remote_head_ref), "refs/remotes/%s/HEAD", remote_name);

  struct object_id oid_temp;
  int ref_flags = 0;
  const char *resolved_ref =
    refs_resolve_ref_unsafe(ctx->refs, remote_head_ref, RESOLVE_REF_READING, &oid_temp, &ref_flags);
  if (debug_mode) {
    fprintf(stderr, "[DEBUG] resolved_ref = %s\n", resolved_ref ? resolved_ref : "(null)");
  }

  if (resolved_ref && skip_prefix(resolved_ref, "refs/remotes/", &main_branch)) {
    /* Successfully resolved <remote>/HEAD to something like "origin/main" */
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] After skip_prefix: main_branch = %s\n",
              main_branch ? main_branch : "(null)");
    }

    /* Copy the string since resolved_ref may be invalidated by later git calls */
    if (main_branch) {
      main_branch_allocated = strdup(main_branch);
      main_branch = main_branch_allocated;
      main_from_symref = 1; /* Came from origin/HEAD symref */
    }
  } else {
    /* No <remote>/HEAD configured - try common fallbacks (main, master) */
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] No refs/remotes/%s/HEAD - trying fallbacks\n", remote_name);
    }

    /* Try <remote>/main first */
    struct strbuf fallback = STRBUF_INIT;
    strbuf_addf(&fallback, "%s/main", remote_name);
    struct strbuf fallback_ref = STRBUF_INIT;
    strbuf_addf(&fallback_ref, "refs/remotes/%s", fallback.buf);

    if (refs_ref_exists(ctx->refs, fallback_ref.buf)) {
      main_branch_allocated = strbuf_detach(&fallback, NULL);
      main_branch = main_branch_allocated;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] Using fallback: %s\n", main_branch);
      }
    } else {
      /* Try <remote>/master */
      strbuf_reset(&fallback);
      strbuf_addf(&fallback, "%s/master", remote_name);
      strbuf_reset(&fallback_ref);
      strbuf_addf(&fallback_ref, "refs/remotes/%s", fallback.buf);

      if (refs_ref_exists(ctx->refs, fallback_ref.buf)) {
        main_branch_allocated = strbuf_detach(&fallback, NULL);
        main_branch = main_branch_allocated;
        if (debug_mode) {
          fprintf(stderr, "[DEBUG] Using fallback: %s\n", main_branch);
        }
      } else if (debug_mode) {
        fprintf(stderr, "[DEBUG] No fallback refs found - skipping distance\n");
      }
    }

    strbuf_release(&fallback);
    strbuf_release(&fallback_ref);
  }

  if (debug_mode) {
    fprintf(stderr, "[DEBUG] main_branch = %s\n", main_branch ? main_branch : "(null)");
  }

  struct object_id main_oid;
  int has_main_oid = 0;

  if (main_branch && !repo_get_oid(the_repository, main_branch, &main_oid)) {
    has_main_oid = 1;
  }

  /*
	 * Phase 2: Check distance from upstream tracking branch
	 * Skip if upstream is the same as main_branch (avoid redundant indicators)
	 */
  struct object_id upstream_oid;
  int has_upstream = 0;
  int upstream_is_main = 0;

  /* Use branch API to get upstream tracking branch (reuse current_branch from Phase 1) */
  const char *upstream = NULL;
  if (current_branch) {
    upstream = branch_get_upstream(current_branch, NULL);
  }

  if (debug_mode) {
    fprintf(stderr, "[DEBUG] upstream = %s, has_upstream = %d\n", upstream ? upstream : "(null)",
            has_upstream);
  }

  if (upstream && !repo_get_oid(the_repository, upstream, &upstream_oid)) {
    has_upstream = 1;

    if (debug_mode) {
      fprintf(stderr, "[DEBUG] Successfully got upstream OID\n");
    }

    /* Check if upstream is the same as main_branch */
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] About to compare: main_branch=%s\n",
              main_branch ? main_branch : "(null)");
    }

    if (has_main_oid && oideq(&upstream_oid, &main_oid)) {
      upstream_is_main = 1;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] upstream_is_main = 1 (upstream matches "
                        "main_branch)\n");
      }
    }
    if (debug_mode && !upstream_is_main) {
      fprintf(stderr, "[DEBUG] upstream_is_main = 0\n");
    }
  }

  /*
	 * Compute distances using BFS (automatically cached internally).
	 */
  int main_ahead = -1, main_behind = -1;
  int upstream_ahead = -1, upstream_behind = -1;

  /* Compute main distance */
  if (has_main_oid) {
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] BFS: HEAD = %s\n", oid_to_hex(&ctx->oid));
      fprintf(stderr, "[DEBUG] BFS: %s = %s\n", main_branch, oid_to_hex(&main_oid));
    }
    struct bfs_distance_result main_result =
      bfs_find_distance(&ctx->oid, &main_oid, max_traversal, debug_mode);
    main_ahead = main_result.ahead;
    main_behind = main_result.behind;
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] main distance: ahead=%d, behind=%d, cost=%d\n", main_ahead,
              main_behind, main_result.commits_visited);
    }
  }

  /* Compute upstream distance (if different from main) */
  if (has_upstream && !upstream_is_main) {
    if (debug_mode) {
      fprintf(stderr, "[DEBUG] BFS: upstream = %s = %s\n", upstream, oid_to_hex(&upstream_oid));
    }
    struct bfs_distance_result upstream_result =
      bfs_find_distance(&ctx->oid, &upstream_oid, max_traversal, debug_mode);
    upstream_ahead = upstream_result.ahead;
    upstream_behind = upstream_result.behind;
    if (debug_mode) {
      fprintf(stderr,
              "[DEBUG] upstream distance: ahead=%d, behind=%d, "
              "cost=%d\n",
              upstream_ahead, upstream_behind, upstream_result.commits_visited);
    }
  }

  DEBUG_TIMER_END(distance, "Distance check");

  /*
	 * Display strategy: Show two separate indicators
	 * 1. Relationship to origin/master (main codebase)
	 * 2. Relationship to upstream tracking branch
	 */

  /* Show main distance when main_branch exists */
  /* Skip if we already showed it as upstream indicator AND would be duplicate */
  if (main_branch && (!has_upstream || !upstream_is_main)) {
    if (main_ahead >= 0 && main_behind >= 0) {
      /* Both values known - found merge-base, can show accurate distance */
      if (main_ahead > 0 && main_behind > 0) {
        /* Diverged: both ahead and behind - never use parentheses */
        strbuf_color_addf(indicators, COLOR_DIVERGED, "↑%d↓%d", main_ahead, main_behind);
      } else if (main_ahead > 0) {
        /* Ahead of main - never use parentheses */
        strbuf_color_addf(indicators, COLOR_AHEAD, "↑%d", main_ahead);
      } else if (main_behind > 0) {
        /* Behind main - never use parentheses */
        strbuf_color_addf(indicators, COLOR_BEHIND, "↓%d", main_behind);
      }
      /* If both are 0, we're in sync - don't show anything */
    } else {
      /* Both searches exhausted - too far apart */
      strbuf_color_addf(indicators, COLOR_DIVERGED, "↕");
    }
  }

  /* Show upstream == main distance with appropriate formatting */
  if (has_upstream && upstream_is_main) {
    /*
		 * Use parentheses if main came from fallback (not from origin/HEAD symref).
		 * When origin/HEAD symref exists and points to same as upstream, treat as
		 * canonical main branch (no parentheses). Otherwise use parentheses to
		 * indicate upstream tracking relationship.
		 */
    int use_parens = !main_from_symref;

    if (main_ahead >= 0 && main_behind >= 0) {
      if (main_ahead > 0 && main_behind > 0) {
        if (use_parens) {
          strbuf_color_addf(indicators, COLOR_DIVERGED, "(↑%d↓%d)", main_ahead, main_behind);
        } else {
          strbuf_color_addf(indicators, COLOR_DIVERGED, "↑%d↓%d", main_ahead, main_behind);
        }
      } else if (main_ahead > 0) {
        if (use_parens) {
          strbuf_color_addf(indicators, COLOR_AHEAD, "(↑%d)", main_ahead);
        } else {
          strbuf_color_addf(indicators, COLOR_AHEAD, "↑%d", main_ahead);
        }
      } else if (main_behind > 0) {
        if (use_parens) {
          strbuf_color_addf(indicators, COLOR_BEHIND, "(↓%d)", main_behind);
        } else {
          strbuf_color_addf(indicators, COLOR_BEHIND, "↓%d", main_behind);
        }
      }
    } else {
      if (use_parens) {
        strbuf_color_addf(indicators, COLOR_DIVERGED, "(↕)");
      } else {
        strbuf_color_addf(indicators, COLOR_DIVERGED, "↕");
      }
    }
  }

  /* Show upstream tracking distance */
  /* Skip if upstream == main_branch (we already showed main distance above) */
  if (has_upstream && !upstream_is_main) {
    if (upstream_ahead >= 0 && upstream_behind >= 0) {
      /* Both values known - found merge-base, can show accurate distance */
      if (upstream_ahead > 0 && upstream_behind > 0) {
        /* Diverged from upstream - both ahead and behind */
        strbuf_color_addf(indicators, COLOR_DIVERGED, "(↑%d↓%d)", upstream_ahead, upstream_behind);
      } else if (upstream_ahead > 0) {
        /* Ahead of upstream - need to push */
        strbuf_color_addf(indicators, COLOR_AHEAD, "(↑%d)", upstream_ahead);
      } else if (upstream_behind > 0) {
        /* Behind upstream - need to pull */
        strbuf_color_addf(indicators, COLOR_BEHIND, "(↓%d)", upstream_behind);
      }
      /* If both are 0, we're in sync - don't show anything */
    } else {
      /* Both searches exhausted - too far apart */
      strbuf_color_addf(indicators, COLOR_DIVERGED, "(↕)");
    }
  }

  /* Clean up allocated main_branch string */
  free(main_branch_allocated);
}

/*
 * Section 3: Collect miscellaneous indicators.
 * Includes: detached HEAD, git state (merge/rebase/etc), stash.
 *
 * Takes git_state computed earlier to avoid redundant checks.
 *
 * Performance: O(1) - checks simple flags and ref existence
 * Safe for large repo mode: Yes (no index or worktree operations)
 */
static void get_misc_indicators(struct strbuf *indicators, int detached,
                                const struct prompt_context *ctx, const struct git_state *state)
{
  /* Detached HEAD indicator (emoji, color has no effect) */
  if (detached) {
    strbuf_addstr(indicators, "⚡");
  }

  /* Display git state if present (merge, rebase, cherry-pick, etc.) */
  if (state->has_state) {
    strbuf_color_addf(indicators, state->state_color, "[%s]", state->state_name);
  }

  /* Check for stashed changes (emoji, color has no effect) */
  if (refs_ref_exists(ctx->refs, "refs/stash")) {
    strbuf_addstr(indicators, "💾");
  }
}

int main(int argc, const char **argv)
{
  struct timeval tv_start_total, tv_end_total;
  int no_color = 0;
  int nongit_ok = 0;
  const char *from_commit = NULL;
  const char *to_commit = NULL;
  const struct option options[] = {
    OPT_BOOL(0, "no-color", &no_color, "disable colored output"),
    OPT_BOOL(0, "debug", &debug_mode, "show timing information"),
    OPT_INTEGER(0, "large-repo-size", &large_repo_size,
                "index size threshold for large repo detection (default: 5000000)"),
    OPT_INTEGER(0, "max-traversal", &max_traversal,
                "maximum commits to traverse in distance calculation (default: 1000)"),
    OPT_BOOL(0, "local", &local_mode, "skip reading global git config"),
    OPT_STRING(0, "from", &from_commit, "commit", "start commit for distance calculation"),
    OPT_STRING(0, "to", &to_commit, "commit", "target commit for distance calculation"),
    OPT_END()};
  struct strbuf branch = STRBUF_INIT;
  struct strbuf indicators = STRBUF_INIT;
  const char *branch_color = COLOR_CLEAN;
  int detached = 0;
  struct prompt_context ctx;
  const char *prefix;

  /* Handle --help before parse_options to avoid triggering man page */
  if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
    show_help();
    return 0;
  }

  /* Initialize repository - required for libgit.a functions */
  initialize_repository(the_repository);

  /* Setup git repository */
  prefix = setup_git_directory_gently(&nongit_ok);

  /* Return silently if not in a git repository */
  if (nongit_ok) {
    return 0;
  }

  /* Load git config (needed for core.excludesfile and other settings) */
  /* Skip global config if --local flag is set (useful for tests) */
  if (!local_mode) {
    DEBUG_TIMER_START(config);
    repo_config(the_repository, git_default_config, NULL);
    DEBUG_TIMER_END(config, "Config load");
  }

  argc = parse_options(argc, argv, prefix, options, prompt_usage, 0);

  /* Apply the no-color flag */
  if (no_color) {
    use_color = 0;
  }

  /* Start timing after options are parsed */
  if (debug_mode) {
    gettimeofday(&tv_start_total, NULL);
  }

  /* Handle distance subcommand */
  if (argc > 0 && !strcmp(argv[0], "distance")) {
    struct object_id from_oid, to_oid;

    /* Validate required arguments */
    if (!from_commit || !to_commit) {
      fprintf(stderr, "error: distance subcommand requires --from and --to arguments\n");
      usage_with_options(prompt_usage, options);
    }

    /* Resolve commit OIDs */
    if (repo_get_oid(the_repository, from_commit, &from_oid)) {
      fprintf(stderr, "error: cannot resolve --from commit: %s\n", from_commit);
      return 1;
    }

    if (repo_get_oid(the_repository, to_commit, &to_oid)) {
      fprintf(stderr, "error: cannot resolve --to commit: %s\n", to_commit);
      return 1;
    }

    /* Calculate distance */
    struct bfs_distance_result result =
      bfs_find_distance(&from_oid, &to_oid, max_traversal, debug_mode);

    /* Output in machine-readable format: "ahead,behind" or "ahead,behind:ancestor" if diverged */
    if (result.ahead > 0 && result.behind > 0) {
      printf("%d,%d:%s\n", result.ahead, result.behind, oid_to_hex(&result.ancestor));
    } else {
      printf("%d,%d\n", result.ahead, result.behind);
    }

    return 0;
  }

  if (argc > 0) {
    usage_with_options(prompt_usage, options);
  }

  /* Check if we're in a git repository - exit silently if not */
  if (!the_repository || !the_repository->gitdir) {
    return 0;
  }

  /* Check if HEAD exists */
  if (repo_get_oid(the_repository, "HEAD", &ctx.oid)) {
    return 0;
  }

  /* Initialize shared context */
  ctx.large_repo = is_large_repo();
  ctx.refs = get_main_ref_store(the_repository);
  ctx.index_loaded = 0;

  /*
	 * Load the index once at the start for all operations.
	 * This avoids multiple expensive index reads throughout the function.
	 *
	 * For large repos: normally skip index loading for performance, BUT
	 * if a git operation is in progress (merge/rebase/etc), we NEED to
	 * load the index to detect conflicts. Checking for state files is O(1),
	 * and conflicts are critical information that must always be accurate.
	 */
  if (!ctx.large_repo) {
    DEBUG_TIMER_START(index);
    if (repo_read_index(the_repository) >= 0) {
      ctx.index_loaded = 1;
    }
    DEBUG_TIMER_END(index, "Index load");
  } else if (has_git_state_files()) {
    /* Large repo with git operation in progress - load index for conflict detection */
    DEBUG_TIMER_START(index);
    if (repo_read_index(the_repository) >= 0) {
      ctx.index_loaded = 1;
      if (debug_mode) {
        fprintf(stderr, "[DEBUG] Large repo: loaded index for conflict "
                        "detection (git operation in progress)\n");
      }
    }
    DEBUG_TIMER_END(index, "Index load");
  }

  /*
	 * Get git state first (merge, rebase, cherry-pick, etc.).
	 * This is needed by branch color determination to detect conflicts.
	 * We compute it once and reuse it for both color and display.
	 */
  struct git_state state = get_git_state(ctx.index_loaded);

  /* Section 1: Get branch name and color */
  detached = get_branch_name_and_color(&branch, &branch_color, &ctx, &state);

  /* Section 3: Get misc indicators (detached, git state, stash) */
  get_misc_indicators(&indicators, detached, &ctx, &state);

  /* Section 2: Get tracking indicators (upstream, distance from main) */
  get_tracking_indicators(&indicators, detached, &branch, &ctx);

  /* Output the prompt */
  color_printf(branch_color, "[%s]", branch.buf);

  if (indicators.len) {
    printf(" %s", indicators.buf);
  }
  printf(" ");

  strbuf_release(&branch);
  strbuf_release(&indicators);

  if (debug_mode) {
    gettimeofday(&tv_end_total, NULL);
    long usec = (tv_end_total.tv_sec - tv_start_total.tv_sec) * 1000000 +
                (tv_end_total.tv_usec - tv_start_total.tv_usec);
    fprintf(stderr, "[DEBUG] Total: %.3fms\n", usec / 1000.0);
  }
  return 0;
}
