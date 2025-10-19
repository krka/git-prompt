#ifndef GRAPH_DISTANCE_CACHE_H
#define GRAPH_DISTANCE_CACHE_H

#include "git-compat-util.h"
#include "object.h"

/*
 * Result of cache lookup for a commit pair.
 * Cache directory: .git/distance-cache/
 * Cache filename: <oid1>-<oid2> (alphabetically sorted)
 * Cache contents: "ahead,behind" (normalized based on sorted order)
 */
struct cache_result {
	int found;  /* 1 if cached, 0 if cache miss */
	int ahead;  /* Commits in oid1 but not in oid2 */
	int behind; /* Commits in oid2 but not in oid1 */
};

/*
 * Try to read cached distance between two commits from .git/distance-cache/
 *
 * Performance: O(1) - single file read and parse
 * Safe for large repo mode: Yes (simple file I/O)
 *
 * Automatically normalizes the commit pair (sorts OIDs alphabetically)
 * and swaps ahead/behind values if needed to match the requested order.
 *
 * Parameters:
 *   oid1  - First commit OID
 *   oid2  - Second commit OID
 *   debug - Enable debug output to stderr
 *
 * Returns: {found=1, ahead, behind} if cached, {found=0, -1, -1} if miss
 */
struct cache_result read_distance_cache(const struct object_id *oid1,
					const struct object_id *oid2, int debug);

/*
 * Write distance between two commits to cache
 * Only writes if traversal cost >= 10 (i.e., BFS visited >= 10 commits)
 *
 * Performance: O(1) - single file write + LRU cleanup (deletes oldest if > 100 files)
 * Safe for large repo mode: Yes (simple file I/O)
 *
 * Automatically normalizes the commit pair (sorts OIDs alphabetically)
 * and swaps ahead/behind values to match the normalized order.
 *
 * Creates .git/distance-cache/ directory if it doesn't exist.
 * Prunes oldest cache files when total count exceeds 100.
 *
 * Parameters:
 *   oid1       - First commit OID
 *   oid2       - Second commit OID
 *   ahead      - Commits in oid1 but not in oid2
 *   behind     - Commits in oid2 but not in oid1
 *   total_cost - Number of commits visited by BFS
 *   debug      - Enable debug output to stderr
 */
void write_distance_cache(const struct object_id *oid1, const struct object_id *oid2,
			  int ahead, int behind, int total_cost, int debug);

#endif /* GRAPH_DISTANCE_CACHE_H */
