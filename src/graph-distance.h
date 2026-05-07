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
 * Two-phase merge-base: fast BFS for approximate answer, then
 * timestamp-bounded LCA search for the exact answer.
 *
 * Phase 1: Bidirectional BFS finds a valid common ancestor A_approx.
 *          Fast (<1ms) but may overshoot on merge-heavy DAGs.
 *
 * Phase 2: Priority-queue LCA search (newest-first by committer timestamp).
 *          Paints commits from both sides, stops when timestamp drops below
 *          A_approx's timestamp minus a safety margin. The most recent
 *          common ancestor found is the true LCA.
 *
 * Returns:
 *   - ancestor is the true LCA (or best approximation within budget)
 *   - ahead/behind are set to 0 (not computed in merge-base mode)
 *   - commits_visited is the total across both phases
 */
struct bfs_distance_result bfs_find_merge_base(const struct object_id *start,
                                               const struct object_id *target, int max_steps,
                                               int debug);

#endif /* GRAPH_DISTANCE_H */
