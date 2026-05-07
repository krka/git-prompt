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
 * Algorithm:
 *
 *   Phase 1 — Bidirectional BFS (same as bfs_find_distance).
 *     Finds *a* common ancestor quickly by growing BFS waves from both
 *     sides until they intersect. On merge-heavy DAGs the intersection
 *     may not be the lowest common ancestor (LCA): BFS minimizes total
 *     edge count, not topological recency.
 *
 *   Phase 2 — Timestamp-bounded LCA refinement.
 *     Uses a priority queue ordered by committer timestamp (newest first)
 *     and paints commits with SIDE_A / SIDE_B flags. When a commit is
 *     painted from both sides it is a common ancestor; the most recent
 *     one found is kept as `best`. The search stops when:
 *       (a) all remaining commits are older than `best`, or
 *       (b) all remaining commits are older than the timestamp barrier
 *           (Phase 1 ancestor date minus BARRIER_MARGIN), or
 *       (c) the visit budget (max_steps * 20) is exhausted.
 *     If Phase 2 finds no better ancestor, the Phase 1 result is used.
 *
 * Correctness:
 *   The result is the true LCA provided:
 *     date(true LCA) >= date(Phase 1 ancestor) - BARRIER_MARGIN
 *   Phase 1 typically finds an ancestor at least as old as the LCA
 *   (BFS overshoots deeper), so the barrier is usually permissive.
 *   The only failure mode is a timestamp inversion on the path between
 *   the Phase 1 result and the true LCA that exceeds BARRIER_MARGIN.
 *   In that case a valid but non-optimal common ancestor is returned.
 *
 * Failure modes:
 *   - max_steps too low: Phase 1 finds no common ancestor at all.
 *     Returns {-1, -1, null_oid}.
 *   - Timestamp inversion > BARRIER_MARGIN on the critical path:
 *     Phase 2 misses the true LCA, returns Phase 1's approximation.
 *     The result is still a valid common ancestor, just not optimal.
 *   - Phase 2 budget exhausted: uses best ancestor found so far.
 *
 * Returns:
 *   - ancestor: best common ancestor found
 *   - ahead/behind: shortest distance from start/target to ancestor
 *   - commits_visited: total across both phases
 */
struct bfs_distance_result bfs_find_merge_base(const struct object_id *start,
                                               const struct object_id *target, int max_steps,
                                               int debug);

#endif /* GRAPH_DISTANCE_H */
