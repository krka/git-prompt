#ifndef GRAPH_DISTANCE_H
#define GRAPH_DISTANCE_H

#include "git-compat-util.h"
#include "commit.h"
#include "oidset.h"

/*
 * Result of bidirectional BFS distance calculation.
 */
struct bfs_distance_result {
  int ahead;                 /* Commits in start but not in target (-1 if not found) */
  int behind;                /* Commits in target but not in start (-1 if not found) */
  int commits_visited;       /* Number of commits traversed (traversal cost) */
  struct object_id ancestor; /* Common ancestor (merge-base) commit */
};

/*
 * Interleaved bidirectional BFS to find distance between two commits.
 *
 * Strategy:
 * 1. Maintain two queues (indexed 0=start, 1=target) for perfect interleaving
 * 2. Track distances from both sides in a single hashmap
 * 3. Alternate: process one from each queue in round-robin fashion
 * 4. When we visit a node that has already been reached from the other side, we found the merge-base
 * 5. Result: ahead = dist_from_start, behind = dist_from_target at intersection
 *
 * Performance: O(commits) where commits ≤ 2 * max_steps
 *              Limited by max_traversal parameter (default 1000)
 *              Early termination when merge-base found
 * Safe for large repo mode: Yes (graph traversal independent of worktree/index size)
 *
 * Parameters:
 *   start     - Usually HEAD commit
 *   target    - Usually origin/master or upstream commit
 *   max_steps - Maximum number of commits to traverse per side
 *   debug     - Enable debug output to stderr
 *
 * Returns:
 *   - {ahead, behind, ancestor} if relationship found within max_steps (both >= 0)
 *   - {-1, -1, null_oid} if too far apart (no common ancestor within max_steps)
 */
struct bfs_distance_result bfs_find_distance(const struct object_id *start,
                                             const struct object_id *target, int max_steps,
                                             int debug);

/*
 * Priority-queue merge-base finder.
 *
 * Algorithm:
 *   Seed both tips into a max-heap ordered by commit date. Pop the
 *   newest commit, paint its parents with the same side flag (A or B).
 *   The first commit that receives both flags is the most recent common
 *   ancestor (merge-base). For max=1, terminate immediately — the
 *   max-heap ordering guarantees no higher common ancestor exists.
 *
 * Correctness:
 *   Equivalent to git's paint_down_to_common with early termination.
 *   The max-heap processes commits in decreasing date order, so the
 *   first doubly-painted commit is the best LCA. Correct as long as
 *   commit dates are monotonic (parents older than children). Clock
 *   skew can cause a non-optimal result — same limitation as git's
 *   own commit-date-ordered merge-base when generation numbers are
 *   unavailable.
 *
 * Failure modes:
 *   - max_steps too low: budget exhausted before finding the LCA.
 *     Returns {-1, -1, null_oid}.
 *
 * Returns:
 *   - ancestor: best common ancestor found
 *   - ahead/behind: BFS hop count from start/target to ancestor
 *   - commits_visited: total commits popped from the priority queue
 */
struct bfs_distance_result bfs_find_merge_base(const struct object_id *start,
                                               const struct object_id *target, int max_steps,
                                               int debug,
                                               const struct oidset *exclude);

#endif /* GRAPH_DISTANCE_H */
