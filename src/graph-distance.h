#ifndef GRAPH_DISTANCE_H
#define GRAPH_DISTANCE_H

#include "git-compat-util.h"
#include "commit.h"

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
 * Two-phase merge-base finder.
 *
 * Phase 1: Bidirectional BFS finds a valid common ancestor quickly.
 *          May overshoot on merge-heavy DAGs (finds *a* common ancestor,
 *          not necessarily the best one).
 *
 * Phase 2: Priority-queue LCA search (newest-first by committer timestamp).
 *          Explores commits from both sides, looking for the most recent
 *          common ancestor. Stops when timestamps drop below Phase 1's
 *          ancestor timestamp minus a 1-hour safety margin.
 *
 * Limitations:
 *   - If max_steps is too low for Phase 1 to find any common ancestor,
 *     returns {-1, -1, null_oid} (no result).
 *   - If committer timestamps are severely out of order (by more than
 *     1 hour), the timestamp barrier may prune the true LCA and Phase 2
 *     falls back to Phase 1's approximation.
 *   - Phase 2 budget is max_steps * 20; if exhausted, uses the best
 *     ancestor found so far (which may be Phase 1's result).
 *
 * Returns:
 *   - ancestor: best common ancestor found (exact in typical repos)
 *   - ahead/behind: shortest distance from start/target to ancestor
 *   - commits_visited: total across both phases
 */
struct bfs_distance_result bfs_find_merge_base(const struct object_id *start,
                                               const struct object_id *target, int max_steps,
                                               int debug);

#endif /* GRAPH_DISTANCE_H */
