#ifndef GRAPH_TRAVERSAL_H
#define GRAPH_TRAVERSAL_H

#include "git-compat-util.h"
#include "commit.h"

/*
 * Result of bidirectional BFS divergence calculation.
 */
struct bfs_divergence_result {
	int ahead;	     /* Commits in start but not in target (-1 if not found) */
	int behind;	     /* Commits in target but not in start (-1 if not found) */
	int commits_visited; /* Number of commits traversed (traversal cost) */
};

/*
 * Interleaved bidirectional BFS to find divergence between two commits.
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
 *   - {ahead, behind} if relationship found within max_steps (both >= 0)
 *   - {-1, -1} if too far apart (no common ancestor within max_steps)
 */
struct bfs_divergence_result bfs_find_divergence(const struct object_id *start,
						 const struct object_id *target, int max_steps,
						 int debug);

#endif /* GRAPH_TRAVERSAL_H */
