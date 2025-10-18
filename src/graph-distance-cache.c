/*
 * graph-distance-cache.c - File-based caching for BFS divergence results
 */
#define USE_THE_REPOSITORY_VARIABLE

#include "graph-distance-cache.h"
#include "git-compat-util.h"
#include "repository.h"
#include "strbuf.h"
#include "hex.h"
#include <stdio.h>

/*
 * Build cache key string from OIDs.
 * Format: <head_oid>,<remote_oid>,<tracking_oid>
 * Empty strings for missing refs.
 */
void build_cache_key(struct strbuf *key, const struct object_id *head_oid,
		     const struct object_id *remote_oid, const struct object_id *tracking_oid,
		     int has_remote, int has_tracking)
{
	strbuf_addstr(key, oid_to_hex(head_oid));
	strbuf_addch(key, ',');
	if (has_remote) {
		strbuf_addstr(key, oid_to_hex(remote_oid));
	}
	strbuf_addch(key, ',');
	if (has_tracking) {
		strbuf_addstr(key, oid_to_hex(tracking_oid));
	}
}

/*
 * Try to read cached divergence data from .git/prompt-cache
 * Performance: O(1) - single file read and parse
 * Safe for large repo mode: Yes (simple file I/O)
 *
 * Returns cache data with cached=0 if cache miss, cached=1 if cache hit
 */
struct divergence_data read_divergence_cache(const struct strbuf *cache_key, int debug)
{
	struct divergence_data data = {0, -1, -1, -1, -1};
	struct strbuf cache_path = STRBUF_INIT;
	struct strbuf line = STRBUF_INIT;
	FILE *fp;
	int ma, mb, ua, ub;

	strbuf_addf(&cache_path, "%s/prompt-cache", repo_get_git_dir(the_repository));
	fp = fopen(cache_path.buf, "r");
	if (!fp) {
		goto cleanup;
	}

	/* Read the single line */
	if (strbuf_getline(&line, fp) == EOF) {
		goto cleanup;
	}

	/* Check if cache key matches (substring match before '=') */
	if (!starts_with(line.buf, cache_key->buf) || line.buf[cache_key->len] != '=') {
		goto cleanup;
	}

	/* Parse values after '=' */
	if (sscanf(line.buf + cache_key->len + 1, "%d,%d,%d,%d", &ma, &mb, &ua, &ub) != 4) {
		goto cleanup;
	}

	/* Cache hit! */
	data.cached = 1;
	data.main_ahead = ma;
	data.main_behind = mb;
	data.upstream_ahead = ua;
	data.upstream_behind = ub;

	if (debug) {
		fprintf(stderr, "[DEBUG] Cache: HIT (main=%d↑%d↓, upstream=%d↑%d↓)\n", ma, mb, ua, ub);
	}

cleanup:
	if (!data.cached && debug) {
		fprintf(stderr, "[DEBUG] Cache: MISS (computing divergence)\n");
	}

	if (fp) {
		fclose(fp);
	}
	strbuf_release(&cache_path);
	strbuf_release(&line);
	return data;
}

/*
 * Write divergence data to cache atomically
 * Only writes if traversal cost >= 10 (i.e., BFS visited >= 10 commits)
 *
 * Performance: O(1) - single file write (atomic via temp file + rename)
 * Safe for large repo mode: Yes (simple file I/O)
 */
void write_divergence_cache(const struct strbuf *cache_key, const struct divergence_data *data,
			     int total_cost, int debug)
{
	struct strbuf cache_path = STRBUF_INIT;
	struct strbuf temp_path = STRBUF_INIT;
	FILE *fp;

	/*
	 * Only cache if BFS was expensive (visited >= 10 commits total).
	 * This avoids writing cache for trivial cases while capturing expensive traversals.
	 */
	if (total_cost < 10) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: SKIP_WRITE (total_cost=%d commits visited)\n",
				total_cost);
		}
		return;
	}

	/* Atomic write: temp file + rename */
	strbuf_addf(&cache_path, "%s/prompt-cache", repo_get_git_dir(the_repository));
	strbuf_addf(&temp_path, "%s.tmp", cache_path.buf);

	fp = fopen(temp_path.buf, "w");
	if (!fp) {
		goto cleanup;
	}

	/* Format: <cache_key>=<ma>,<mb>,<ua>,<ub> */
	fprintf(fp, "%s=%d,%d,%d,%d\n", cache_key->buf, data->main_ahead, data->main_behind,
		data->upstream_ahead, data->upstream_behind);

	fclose(fp);

	/* Atomic rename */
	if (rename(temp_path.buf, cache_path.buf) == 0) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: WRITE (total_cost=%d commits visited)\n",
				total_cost);
		}
	}

cleanup:
	strbuf_release(&cache_path);
	strbuf_release(&temp_path);
}
