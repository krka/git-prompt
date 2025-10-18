#ifndef GRAPH_DISTANCE_CACHE_H
#define GRAPH_DISTANCE_CACHE_H

#include "git-compat-util.h"
#include "object.h"

/*
 * Cached divergence data for BFS results.
 * Cache format: <head_oid>,<remote_oid>,<tracking_oid>=<main_ahead>,<main_behind>,<upstream_ahead>,<upstream_behind>
 * Empty string represents missing ref (e.g., no tracking branch)
 */
struct divergence_data {
	int cached; /* 1 if data is from cache or valid, 0 if cache miss */
	int main_ahead;
	int main_behind;
	int upstream_ahead;
	int upstream_behind;
};

/*
 * Build cache key string from OIDs.
 * Format: <head_oid>,<remote_oid>,<tracking_oid>
 * Empty strings for missing refs.
 */
void build_cache_key(struct strbuf *key, const struct object_id *head_oid,
		     const struct object_id *remote_oid, const struct object_id *tracking_oid,
		     int has_remote, int has_tracking);

/*
 * Try to read cached divergence data from .git/prompt-cache
 * Performance: O(1) - single file read and parse
 * Safe for large repo mode: Yes (simple file I/O)
 *
 * Parameters:
 *   cache_key - Key to look up in cache
 *   debug     - Enable debug output to stderr
 *
 * Returns cache data with cached=0 if cache miss, cached=1 if cache hit
 */
struct divergence_data read_divergence_cache(const struct strbuf *cache_key, int debug);

/*
 * Write divergence data to cache atomically
 * Only writes if traversal cost >= 10 (i.e., BFS visited >= 10 commits)
 *
 * Performance: O(1) - single file write (atomic via temp file + rename)
 * Safe for large repo mode: Yes (simple file I/O)
 *
 * Parameters:
 *   cache_key  - Key to write to cache
 *   data       - Divergence data to cache
 *   total_cost - Number of commits visited by BFS
 *   debug      - Enable debug output to stderr
 */
void write_divergence_cache(const struct strbuf *cache_key, const struct divergence_data *data,
			     int total_cost, int debug);

#endif /* GRAPH_DISTANCE_CACHE_H */
