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
#include "repo-settings.h"
#include "strbuf.h"
#include "parse-options.h"
#include "read-cache.h"
#include "cache-tree.h"
#include "wt-status.h"
#include "dir.h"
#include "oidset.h"
#include "remote.h"
#include "hex.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>

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
 * - get_tracking_indicators()    O(commits)- merge-base (fast with early-exit) + small ahead walk
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
  "[--max-traversal=<commits>] [--local]", NULL};

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
  "  (↓Xt)    - Behind upstream by time t (yellow - need to pull)\n"
  "  (↑N ↓Xt) - N commits ahead, Xt behind (diverged, red)\n"
  "  (nothing shown when in sync with upstream)\n"
  "\n"
  "DISTANCE FROM MAIN (shown for feature branches):\n"
  "  ↑N       - N commits ahead of origin/main or origin/master (blue)\n"
  "  ↓Xt      - Behind origin/main by time t (yellow)\n"
  "  ↑N ↓Xt   - N commits ahead, Xt behind\n"
  "\n"
  "  Time units: s (seconds), m (minutes), h (hours), d (days), w (weeks), mo (months)\n"
  "\n"
  "WORKTREE INDICATORS:\n"
  "  🌳       - Linked worktree (full checkout)\n"
  "  🪾       - Sparse worktree (partial checkout)\n"
  "\n"
  "OTHER INDICATORS:\n"
  "  💾       - Stashed changes present (cyan)\n"
  "\n"
  "EXAMPLES:\n"
  "  [main]                - On main, in sync with upstream, clean\n"
  "  [main] (↑2)           - On main, 2 commits ahead of upstream\n"
  "  [feature] ↑5 ↓3d      - 5 commits ahead, 3 days behind main\n"
  "  [feature] ↑10(↑2)     - 10 ahead of main, 2 unpushed to upstream\n"
  "  [main] ⚡ [merge:conflict]  - Detached HEAD, merge with conflicts\n"
  "  [feature] 💾          - On feature, has stashed changes\n"
  "\n"
  "PERFORMANCE:\n"
  "  For large repositories (>5MB index), status checks are skipped for speed.\n"
  "  Uses git's merge-base with early-exit optimization for fast ahead/behind.\n"
  "  Ahead count limited by --max-traversal (default 1000).\n"
  "  Behind shown as time delta (free from commit timestamps).\n"
  "\n"
  "SHELL INTEGRATION:\n"
  "  Bash:  PS1='$(git prompt)\\$ '\n"
  "  Zsh:   setopt PROMPT_SUBST; PROMPT='$(git prompt)%% '\n"
  "  Fish:  function fish_prompt; git prompt; end\n";

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

  /* Check the worktree's own index */
  strbuf_addf(&index_file, "%s/index", repo_get_git_dir(the_repository));

  if (!stat(index_file.buf, &st) && st.st_size > large_repo_size) {
    strbuf_release(&index_file);
    return 1;
  }

  /*
   * For linked worktrees (especially sparse checkouts), the worktree's
   * index may be small while the main repo is huge. Check the main
   * repo's index too — it reflects the true repository scale.
   */
  if (the_repository->different_commondir) {
    strbuf_reset(&index_file);
    strbuf_addf(&index_file, "%s/index", repo_get_common_dir(the_repository));
    if (!stat(index_file.buf, &st) && st.st_size > large_repo_size) {
      strbuf_release(&index_file);
      return 1;
    }
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
  int msgnum;              /* Current commit number (for rebase) */
  int end;                 /* Total commits (for rebase) */
  int remaining;           /* Remaining commits (for cherry-pick/revert sequences) */
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

    /* Skip sparse directory entries and skip-worktree entries — no file on disk to stat */
    if (S_ISSPARSEDIR(ce->ce_mode) || ce_skip_worktree(ce)) {
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
 * Read rebase progress information from msgnum and end files.
 * These files track which commit is currently being processed and the total count.
 *
 * Performance: O(1) - reads two small files
 * Safe for large repo mode: Yes (no index operations)
 *
 * Parameters:
 *   gitdir - Git directory path
 *   rebase_dir - Name of the rebase directory ("rebase-merge" or "rebase-apply")
 *   msgnum - Output: current commit number (1-based)
 *   end - Output: total number of commits
 */
static void read_rebase_progress(const char *gitdir, const char *rebase_dir, int *msgnum, int *end)
{
  struct strbuf path = STRBUF_INIT;
  struct strbuf content = STRBUF_INIT;

  /* Read msgnum (current commit) - rebase-merge uses "msgnum", rebase-apply uses "next" */
  strbuf_addf(&path, "%s/%s/msgnum", gitdir, rebase_dir);
  if (strbuf_read_file(&content, path.buf, 0) > 0) {
    *msgnum = atoi(content.buf);
  } else {
    /* Try "next" for git am (rebase-apply) */
    strbuf_reset(&path);
    strbuf_reset(&content);
    strbuf_addf(&path, "%s/%s/next", gitdir, rebase_dir);
    if (strbuf_read_file(&content, path.buf, 0) > 0) {
      *msgnum = atoi(content.buf);
    }
  }

  /* Read end (total commits) - rebase-merge uses "end", rebase-apply uses "last" */
  strbuf_reset(&path);
  strbuf_reset(&content);
  strbuf_addf(&path, "%s/%s/end", gitdir, rebase_dir);
  if (strbuf_read_file(&content, path.buf, 0) > 0) {
    *end = atoi(content.buf);
  } else {
    /* Try "last" for git am (rebase-apply) */
    strbuf_reset(&path);
    strbuf_reset(&content);
    strbuf_addf(&path, "%s/%s/last", gitdir, rebase_dir);
    if (strbuf_read_file(&content, path.buf, 0) > 0) {
      *end = atoi(content.buf);
    }
  }

  strbuf_release(&path);
  strbuf_release(&content);
}

/*
 * Count remaining commits in a cherry-pick or revert sequence.
 * Reads .git/sequencer/todo and counts lines starting with "pick".
 *
 * Performance: O(n) where n = lines in todo file (typically small)
 * Safe for large repo mode: Yes (no index operations)
 *
 * Parameters:
 *   gitdir - Git directory path
 *
 * Returns: Number of remaining commits, or 0 if no sequencer state
 */
static int count_sequencer_remaining(const char *gitdir)
{
  struct strbuf path = STRBUF_INIT;
  struct strbuf content = STRBUF_INIT;
  int count = 0;

  strbuf_addf(&path, "%s/sequencer/todo", gitdir);
  if (strbuf_read_file(&content, path.buf, 0) > 0) {
    const char *line = content.buf;
    while (line && *line) {
      /* Count lines starting with "pick " (cherry-pick/revert commands) */
      if (starts_with(line, "pick ")) {
        count++;
      }
      line = strchr(line, '\n');
      if (line) {
        line++; /* skip newline */
      }
    }
  }

  strbuf_release(&path);
  strbuf_release(&content);
  return count;
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
  struct git_state state = {0, 0, NULL, NULL, 0, 0, 0};
  const char *gitdir = repo_get_git_dir(the_repository);

  /* Check for rebase (interactive or apply mode) */
  if (check_git_state_file(gitdir, "rebase-merge", &state, index_loaded, "rebase:conflict",
                           "rebase:continue")) {
    read_rebase_progress(gitdir, "rebase-merge", &state.msgnum, &state.end);
    return state;
  }

  if (check_git_state_file(gitdir, "rebase-apply", &state, index_loaded, "rebase:conflict",
                           "rebase:continue")) {
    read_rebase_progress(gitdir, "rebase-apply", &state.msgnum, &state.end);
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
    state.remaining = count_sequencer_remaining(gitdir);
    return state;
  }

  /* Check for revert */
  check_git_state_file(gitdir, "REVERT_HEAD", &state, index_loaded, "revert:conflict",
                       "revert:commit");

  /* If revert is in progress, count remaining commits */
  if (state.has_state) {
    state.remaining = count_sequencer_remaining(gitdir);
  }

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

    /*
     * DISABLED: Tag name lookup for detached HEAD
     * This requires loading all refs via get_name_decoration(), which is
     * very expensive in repos with many refs (7000+ refs = 400ms+).
     * Short commit hash is sufficient and fast.
     */
#if 0
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
#endif

    /* Use short commit hash for detached HEAD */
    strbuf_add_unique_abbrev(branch, &ctx->oid, 7);
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
 * Format a time delta as a compact human-readable string.
 * Uses the largest appropriate unit: s, m, h, d, w, mo.
 */
static void format_time_delta(struct strbuf *sb, timestamp_t seconds)
{
  if (seconds < 60)
    strbuf_addf(sb, "%"PRItime"s", seconds);
  else if (seconds < 3600)
    strbuf_addf(sb, "%"PRItime"m", seconds / 60);
  else if (seconds < 86400)
    strbuf_addf(sb, "%"PRItime"h", seconds / 3600);
  else if (seconds < 604800)
    strbuf_addf(sb, "%"PRItime"d", seconds / 86400);
  else if (seconds < 2592000)
    strbuf_addf(sb, "%"PRItime"w", seconds / 604800);
  else
    strbuf_addf(sb, "%"PRItime"mo", seconds / 2592000);
}

#define AHEAD_OVERFLOW -1
#define AHEAD_DISJOINT -2

/*
 * Count commits reachable from 'tip' but not from 'base' by walking parents.
 * Returns AHEAD_OVERFLOW if count exceeds max_count.
 */
static int count_commits_ahead(const struct object_id *tip,
                               const struct object_id *base,
                               int max_count)
{
  struct commit *c;
  struct commit_list *work = NULL;
  struct oidset seen = OIDSET_INIT;
  int count = 0;

  if (oideq(tip, base))
    return 0;

  c = lookup_commit(the_repository, tip);
  if (!c || repo_parse_commit(the_repository, c))
    return AHEAD_OVERFLOW;

  commit_list_insert(c, &work);
  oidset_insert(&seen, tip);

  while (work) {
    struct commit *curr = pop_commit(&work);
    struct commit_list *parent;

    if (oideq(&curr->object.oid, base))
      continue;

    count++;
    if (count > max_count) {
      free_commit_list(work);
      oidset_clear(&seen);
      return AHEAD_OVERFLOW;
    }

    for (parent = curr->parents; parent; parent = parent->next) {
      if (oidset_insert(&seen, &parent->item->object.oid))
        continue;
      if (repo_parse_commit(the_repository, parent->item))
        continue;
      commit_list_insert(parent->item, &work);
    }
  }

  oidset_clear(&seen);
  return count;
}

/*
 * Compute relationship between two refs using merge-base.
 *
 * Uses git's own merge-base (with paint_down_to_common early-exit optimization)
 * then compares OIDs:
 *   - merge-base == head && merge-base == ref: in sync
 *   - merge-base == head: behind only
 *   - merge-base == ref:  ahead only
 *   - otherwise:          diverged (ahead + behind)
 *
 * Ahead count: exact commit count (small, fast to walk).
 * Behind: time delta between merge-base and ref timestamps (free from parsed commits).
 */
struct ref_relationship {
  int ahead;                /* Commits ahead, -1 if too many, -2 if disjoint */
  timestamp_t behind_seconds; /* Seconds behind (0 if not behind) */
  int is_behind;            /* 1 if behind at all */
};

/*
 * Cache file for disjoint history detection.
 * Stores pairs of OIDs that have no common ancestor, so we avoid
 * repeating the expensive full-graph walk on every prompt.
 *
 * Format: one line per pair: "<head_hex> <ref_hex>\n"
 * Location: .git/prompt-disjoint
 */
static void format_oid_pair(struct strbuf *sb,
                            const struct object_id *a,
                            const struct object_id *b)
{
  strbuf_addstr(sb, oid_to_hex(a));
  strbuf_addch(sb, ' ');
  strbuf_addstr(sb, oid_to_hex(b));
}

static int is_cached_disjoint(const struct object_id *head_oid,
                              const struct object_id *ref_oid)
{
  struct strbuf path = STRBUF_INIT;
  struct strbuf content = STRBUF_INIT;
  struct strbuf needle = STRBUF_INIT;
  int found = 0;

  strbuf_addf(&path, "%s/prompt-disjoint", repo_get_git_dir(the_repository));
  if (strbuf_read_file(&content, path.buf, 0) <= 0)
    goto done;

  format_oid_pair(&needle, head_oid, ref_oid);
  found = !!strstr(content.buf, needle.buf);

done:
  strbuf_release(&path);
  strbuf_release(&content);
  strbuf_release(&needle);
  return found;
}

static void cache_disjoint(const struct object_id *head_oid,
                           const struct object_id *ref_oid)
{
  struct strbuf path = STRBUF_INIT;
  struct strbuf line = STRBUF_INIT;
  FILE *f;

  strbuf_addf(&path, "%s/prompt-disjoint", repo_get_git_dir(the_repository));
  f = fopen(path.buf, "a");
  if (f) {
    format_oid_pair(&line, head_oid, ref_oid);
    fprintf(f, "%s\n", line.buf);
    fclose(f);
  }
  strbuf_release(&path);
  strbuf_release(&line);
}

static struct ref_relationship compute_relationship(const struct object_id *head_oid,
                                                    const struct object_id *ref_oid)
{
  struct ref_relationship rel = { 0, 0, 0 };
  struct commit *head_commit, *ref_commit;
  struct commit_list *bases = NULL;
  struct commit *base;

  if (oideq(head_oid, ref_oid))
    return rel;

  if (is_cached_disjoint(head_oid, ref_oid)) {
    if (debug_mode)
      fprintf(stderr, "[DEBUG] Disjoint histories (cached)\n");
    rel.ahead = AHEAD_DISJOINT;
    return rel;
  }

  head_commit = lookup_commit(the_repository, head_oid);
  ref_commit = lookup_commit(the_repository, ref_oid);
  if (!head_commit || !ref_commit)
    return rel;
  if (repo_parse_commit(the_repository, head_commit) ||
      repo_parse_commit(the_repository, ref_commit))
    return rel;

  if (repo_get_merge_bases(the_repository, head_commit, ref_commit,
                           &bases) < 0 ||
      !bases) {
    free_commit_list(bases);
    cache_disjoint(head_oid, ref_oid);
    if (debug_mode)
      fprintf(stderr, "[DEBUG] Disjoint histories — no common ancestor\n");
    rel.ahead = AHEAD_DISJOINT;
    return rel;
  }

  base = bases->item;

  if (!oideq(&base->object.oid, head_oid))
    rel.ahead = count_commits_ahead(head_oid, &base->object.oid, max_traversal);

  if (!oideq(&base->object.oid, ref_oid)) {
    rel.is_behind = 1;
    if (repo_parse_commit(the_repository, base) == 0)
      rel.behind_seconds = ref_commit->date - base->date;
    if (rel.behind_seconds < 0)
      rel.behind_seconds = 0;
  }

  if (debug_mode) {
    fprintf(stderr, "[DEBUG] merge-base: %s, ahead=%d, behind_seconds=%"PRItime"\n",
            oid_to_hex(&base->object.oid), rel.ahead, rel.behind_seconds);
  }

  free_commit_list(bases);
  return rel;
}

/*
 * Section 2: Collect tracking indicators using merge-base.
 *
 * Two-phase approach:
 * Phase 1: Check relationship with origin/master (main codebase)
 * Phase 2: Check relationship with upstream tracking branch (what you pushed)
 *
 * Ahead shown as commit count, behind shown as time duration.
 *
 * Performance: merge-base is fast with paint_down_to_common optimization (~10-60ms).
 *              Ahead count walks only the small local branch (instant for typical branches).
 *              Behind uses timestamp delta (free, already parsed).
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

  /* Always use "origin" as the default remote for main branch detection.
   * This is the canonical upstream in most workflows (including forks).
   * The tracking remote (where you push) may differ from the upstream. */
  const char *remote_name = "origin";

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

  /* Use branch API to get upstream tracking branch */
  const char *upstream = NULL;
  struct branch *current_branch = branch_get(NULL);
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
	 * Compute relationships using merge-base + OID comparison.
	 */
  struct ref_relationship main_rel = { 0, 0, 0 };
  struct ref_relationship upstream_rel = { 0, 0, 0 };

  if (has_main_oid) {
    main_rel = compute_relationship(&ctx->oid, &main_oid);
  }

  if (has_upstream && !upstream_is_main) {
    upstream_rel = compute_relationship(&ctx->oid, &upstream_oid);
  }

  DEBUG_TIMER_END(distance, "Distance check");

  /*
	 * Display strategy: Show two separate indicators
	 * 1. Relationship to origin/master (main codebase) — ahead as count, behind as time
	 * 2. Relationship to upstream tracking branch — ahead as count, behind as time
	 */

  /* Helper macro to emit ahead/behind for a relationship */
#define EMIT_RELATIONSHIP(rel, use_parens, ref_name) do { \
    if ((rel).ahead == AHEAD_DISJOINT) { \
      strbuf_color_addf(indicators, COLOR_DIVERGED, "[no common ancestor with %s]", \
                        ref_name ? ref_name : "remote"); \
    } else if ((rel).ahead || (rel).is_behind) { \
      struct strbuf _tmp = STRBUF_INIT; \
      const char *_color; \
      if ((rel).ahead && (rel).is_behind) \
        _color = COLOR_DIVERGED; \
      else if ((rel).ahead) \
        _color = COLOR_AHEAD; \
      else \
        _color = COLOR_BEHIND; \
      if (use_parens) strbuf_addch(&_tmp, '('); \
      if ((rel).ahead > 0) \
        strbuf_addf(&_tmp, "↑%d", (rel).ahead); \
      else if ((rel).ahead == AHEAD_OVERFLOW) \
        strbuf_addstr(&_tmp, "↑?"); \
      if ((rel).ahead && (rel).is_behind) \
        strbuf_addch(&_tmp, ' '); \
      if ((rel).is_behind) { \
        strbuf_addstr(&_tmp, "↓"); \
        if ((rel).behind_seconds > 0) \
          format_time_delta(&_tmp, (rel).behind_seconds); \
      } \
      if (use_parens) strbuf_addch(&_tmp, ')'); \
      strbuf_color_addf(indicators, _color, "%s", _tmp.buf); \
      strbuf_release(&_tmp); \
    } \
  } while(0)

  /* Show main distance when main_branch exists */
  if (main_branch && (!has_upstream || !upstream_is_main)) {
    EMIT_RELATIONSHIP(main_rel, 0, main_branch);
  }

  /* Show upstream == main distance */
  if (has_upstream && upstream_is_main) {
    int use_parens = !main_from_symref;
    EMIT_RELATIONSHIP(main_rel, use_parens, main_branch);
  }

  /* Show upstream tracking distance (if different from main) */
  if (has_upstream && !upstream_is_main) {
    EMIT_RELATIONSHIP(upstream_rel, 1, upstream);
  }

#undef EMIT_RELATIONSHIP

  /* Clean up allocated main_branch string */
  free(main_branch_allocated);
}

/*
 * Check if git bisect is in progress and format the bisect indicator.
 * Shows good/bad commit counts.
 *
 * Format: [bisect (G/B)] with color coding:
 *   G = number of good commits marked (green)
 *   B = number of bad commits marked (red)
 *
 * Example: [bisect (1/1)] = 1 good, 1 bad
 *
 * Performance: O(k) where k = number of bisect refs
 *              Just counts refs, no graph traversal
 * Safe for large repo mode: Yes (just file operations)
 *
 * Returns 1 if bisect indicator was added, 0 otherwise.
 */
static int get_bisect_indicator(struct strbuf *indicators, const struct prompt_context *ctx)
{
  const char *gitdir = repo_get_git_dir(the_repository);
  struct strbuf path = STRBUF_INIT;
  struct strbuf bisect_refs_dir = STRBUF_INIT;
  DIR *dir;
  struct dirent *entry;
  int good_count = 0;
  int bad_count = 0;

  /* Check if bisect is in progress */
  strbuf_addf(&path, "%s/BISECT_START", gitdir);
  if (access(path.buf, F_OK) != 0) {
    strbuf_release(&path);
    return 0;
  }
  strbuf_release(&path);

  /* Count all good and bad commits */
  strbuf_addf(&bisect_refs_dir, "%s/refs/bisect", gitdir);
  dir = opendir(bisect_refs_dir.buf);
  if (dir) {
    while ((entry = readdir(dir)) != NULL) {
      /* Look for good-* and bad refs */
      if (strncmp(entry->d_name, "good-", 5) == 0) {
        good_count++;
      } else if (strcmp(entry->d_name, "bad") == 0) {
        bad_count++;
      }
    }
    closedir(dir);
  }
  strbuf_release(&bisect_refs_dir);

  /* Format the bisect indicator - simplified to just show good/bad counts */
  if (good_count > 0 || bad_count > 0) {
    /* Simple format: [bisect (good/bad)] with color coding */
    strbuf_addstr(indicators, "[");
    strbuf_color_addf(indicators, COLOR_UNSTAGED, "bisect");
    strbuf_addstr(indicators, " (");
    strbuf_color_addf(indicators, COLOR_CLEAN, "%d", good_count);
    strbuf_addstr(indicators, "/");
    strbuf_color_addf(indicators, COLOR_MODIFIED, "%d", bad_count);
    strbuf_addstr(indicators, ")]");
  } else {
    /* Bisect started but no commits marked yet */
    return 0;
  }

  return 1;
}

/*
 * Section 3: Collect miscellaneous indicators.
 * Includes: detached HEAD, git state (merge/rebase/etc), worktree type, stash, bisect.
 *
 * Takes git_state computed earlier to avoid redundant checks.
 *
 * Performance: O(1) for most indicators, O(k + commits) for bisect
 *              where k = bisect refs, commits bounded by max_traversal
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
    if (state->msgnum > 0 && state->end > 0) {
      /* Rebase progress: show current/total (e.g., "6/11") */
      strbuf_color_addf(indicators, state->state_color, "[%s %d/%d]", state->state_name,
                        state->msgnum, state->end);
    } else if (state->remaining > 0) {
      /* Cherry-pick/revert sequence: show remaining count (e.g., "+8") */
      strbuf_color_addf(indicators, state->state_color, "[%s +%d]", state->state_name,
                        state->remaining);
    } else {
      strbuf_color_addf(indicators, state->state_color, "[%s]", state->state_name);
    }
  }

  /* Check for bisect in progress */
  get_bisect_indicator(indicators, ctx);

  /*
   * Worktree type indicator (emoji, no color)
   * Linked worktrees have different_commondir set (gitdir != commondir).
   * Sparse worktrees additionally have info/sparse-checkout file present.
   * Main checkouts show no indicator (the default state).
   *
   * Performance: O(1) - struct field check + one stat() call
   */
  if (the_repository->different_commondir) {
    struct strbuf sparse_path = STRBUF_INIT;
    strbuf_addf(&sparse_path, "%s/info/sparse-checkout", repo_get_git_dir(the_repository));
    if (file_exists(sparse_path.buf)) {
      strbuf_addstr(indicators, "🪾 ");
    } else {
      strbuf_addstr(indicators, "🌳");
    }
    strbuf_release(&sparse_path);
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
  const struct option options[] = {
    OPT_BOOL(0, "no-color", &no_color, "disable colored output"),
    OPT_BOOL(0, "debug", &debug_mode, "show timing information"),
    OPT_INTEGER(0, "large-repo-size", &large_repo_size,
                "index size threshold for large repo detection (default: 5000000)"),
    OPT_INTEGER(0, "max-traversal", &max_traversal,
                "maximum commits to traverse in ahead count (default: 1000)"),
    OPT_BOOL(0, "local", &local_mode, "skip reading global git config"),
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
  prefix = setup_git_directory_gently(the_repository, &nongit_ok);

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

  if (argc > 0 && !strcmp(argv[0], "merge-base")) {
    fprintf(stderr, "deprecated: use 'git merge-base <commit> <commit>' instead\n");
    return 1;
  }

  if (argc > 0 && !strcmp(argv[0], "distance")) {
    fprintf(stderr, "deprecated: use 'git rev-list --count --left-right <commit>...<commit>' instead\n");
    return 1;
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
   * Tell git we can handle sparse index entries without expansion.
   * All our index operations are sparse-aware:
   * - refresh_index() skips S_ISSPARSEDIR entries
   * - cache_tree_update() uses sparse dir OIDs as leaves
   * - has_unmerged_files() iteration is fine (sparse dirs are stage 0)
   * - fill_directory() walks the filesystem (only sparse cone exists)
   * Without this, repo_read_index() expands sparse → full, which is slow.
   */
  prepare_repo_settings(the_repository);
  the_repository->settings.command_requires_full_index = 0;

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
