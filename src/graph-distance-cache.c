/*
 * graph-distance-cache.c - Directory-based caching for BFS distance results
 *
 * Cache structure:
 *   Directory: .git/distance-cache/
 *   Files: <oid1>-<oid2> where OIDs are sorted alphabetically
 *   Contents: "ahead,behind\n"
 *
 * Normalization: commit pairs are always stored in sorted order,
 * and ahead/behind values are swapped when reading if the requested
 * order differs from the stored order.
 *
 * LRU policy: maintains at most 100 cache files, deleting oldest
 * by modification time when limit is exceeded.
 */
#define USE_THE_REPOSITORY_VARIABLE

#include "graph-distance-cache.h"
#include "git-compat-util.h"
#include "repository.h"
#include "strbuf.h"
#include "hex.h"
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define CACHE_DIR_NAME "distance-cache"
#define MAX_CACHE_FILES 100

/*
 * Normalize a commit pair by sorting OIDs alphabetically.
 * Returns 1 if the OIDs were swapped, 0 if they were already in order.
 */
static int normalize_commit_pair(const struct object_id **oid1, const struct object_id **oid2)
{
	if (oidcmp(*oid1, *oid2) > 0) {
		/* Swap pointers */
		const struct object_id *temp = *oid1;
		*oid1 = *oid2;
		*oid2 = temp;
		return 1; /* Swapped */
	}
	return 0; /* Already in order */
}

/*
 * Build cache file path for a normalized commit pair.
 * Format: .git/distance-cache/<oid1>-<oid2>
 *
 * Assumes oid1 and oid2 are already normalized (sorted).
 */
static void get_cache_filepath(struct strbuf *path, const struct object_id *oid1,
			       const struct object_id *oid2)
{
	strbuf_addf(path, "%s/%s/%s-%s", repo_get_git_dir(the_repository), CACHE_DIR_NAME,
		    oid_to_hex(oid1), oid_to_hex(oid2));
}

/*
 * Helper struct for sorting cache files by modification time.
 */
struct cache_file_info {
	char *path;
	time_t mtime;
};

static int compare_mtime(const void *a, const void *b)
{
	const struct cache_file_info *fa = a;
	const struct cache_file_info *fb = b;
	return (fa->mtime > fb->mtime) - (fa->mtime < fb->mtime);
}

/*
 * Prune oldest cache files when count exceeds MAX_CACHE_FILES.
 * Deletes files with oldest modification time first.
 */
static void prune_old_cache_files(int debug)
{
	struct strbuf cache_dir = STRBUF_INIT;
	DIR *dir;
	struct dirent *entry;
	struct cache_file_info *files = NULL;
	int file_count = 0;
	int capacity = 0;

	strbuf_addf(&cache_dir, "%s/%s", repo_get_git_dir(the_repository), CACHE_DIR_NAME);

	dir = opendir(cache_dir.buf);
	if (!dir) {
		strbuf_release(&cache_dir);
		return;
	}

	/* Collect all cache files with their modification times */
	while ((entry = readdir(dir)) != NULL) {
		struct stat st;
		struct strbuf filepath = STRBUF_INIT;

		if (entry->d_name[0] == '.')
			continue; /* Skip . and .. */

		strbuf_addf(&filepath, "%s/%s", cache_dir.buf, entry->d_name);

		if (stat(filepath.buf, &st) == 0 && S_ISREG(st.st_mode)) {
			/* Grow array if needed */
			if (file_count >= capacity) {
				capacity = capacity ? capacity * 2 : 16;
				files = xrealloc(files, capacity * sizeof(*files));
			}

			files[file_count].path = xstrdup(filepath.buf);
			files[file_count].mtime = st.st_mtime;
			file_count++;
		}

		strbuf_release(&filepath);
	}
	closedir(dir);

	/* If we're under the limit, nothing to do */
	if (file_count <= MAX_CACHE_FILES) {
		goto cleanup;
	}

	/* Sort by modification time (oldest first) */
	qsort(files, file_count, sizeof(*files), compare_mtime);

	/* Delete oldest files until we're at the limit */
	int files_to_delete = file_count - MAX_CACHE_FILES;
	for (int i = 0; i < files_to_delete; i++) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: pruning old file %s\n", files[i].path);
		}
		unlink(files[i].path);
	}

	if (debug) {
		fprintf(stderr, "[DEBUG] Cache: pruned %d old files (had %d, limit %d)\n",
			files_to_delete, file_count, MAX_CACHE_FILES);
	}

cleanup:
	for (int i = 0; i < file_count; i++) {
		free(files[i].path);
	}
	free(files);
	strbuf_release(&cache_dir);
}

/*
 * Try to read cached distance between two commits from .git/distance-cache/
 *
 * Automatically normalizes the commit pair and swaps ahead/behind if needed.
 */
struct cache_result read_distance_cache(const struct object_id *oid1,
					const struct object_id *oid2, int debug)
{
	struct cache_result result;
	const struct object_id *norm_oid1 = oid1;
	const struct object_id *norm_oid2 = oid2;
	struct strbuf cache_path = STRBUF_INIT;
	FILE *fp;
	int ahead, behind;
	char ancestor_hex[GIT_MAX_HEXSZ + 1];
	int swapped;

	/* Initialize result */
	result.found = 0;
	result.ahead = -1;
	result.behind = -1;
	oidcpy(&result.ancestor, null_oid(the_repository->hash_algo));

	/* Normalize the commit pair */
	swapped = normalize_commit_pair(&norm_oid1, &norm_oid2);

	/* Build cache file path */
	get_cache_filepath(&cache_path, norm_oid1, norm_oid2);

	/* Try to open cache file */
	fp = fopen(cache_path.buf, "r");
	if (!fp) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: MISS (%s-%s)\n", oid_to_hex(oid1),
				oid_to_hex(oid2));
		}
		goto cleanup;
	}

	/* Read and parse the "ahead,behind" format */
	if (fscanf(fp, "%d,%d", &ahead, &behind) != 2) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: invalid format in %s\n", cache_path.buf);
		}
		fclose(fp);
		goto cleanup;
	}

	/* Read ancestor OID if both ahead > 0 and behind > 0 (diverged case) */
	if (ahead > 0 && behind > 0) {
		if (fscanf(fp, "%40s", ancestor_hex) == 1) {
			if (get_oid_hex(ancestor_hex, &result.ancestor) < 0) {
				if (debug) {
					fprintf(stderr, "[DEBUG] Cache: invalid ancestor OID in %s\n",
						cache_path.buf);
				}
				fclose(fp);
				goto cleanup;
			}
		}
		/* If ancestor not present, keep as null_oid (already initialized) */
	}
	fclose(fp);

	/* Swap values if we normalized the commit order */
	if (swapped) {
		result.ahead = behind;
		result.behind = ahead;
	} else {
		result.ahead = ahead;
		result.behind = behind;
	}

	result.found = 1;

	if (debug) {
		fprintf(stderr, "[DEBUG] Cache: HIT (%s-%s) = %d,%d\n", oid_to_hex(oid1),
			oid_to_hex(oid2), result.ahead, result.behind);
	}

cleanup:
	strbuf_release(&cache_path);
	return result;
}

/*
 * Write distance between two commits to cache.
 * Only writes if traversal cost >= 10.
 *
 * Automatically normalizes the commit pair and swaps ahead/behind to match.
 * Creates cache directory if needed, and prunes old files when limit exceeded.
 */
void write_distance_cache(const struct object_id *oid1, const struct object_id *oid2, int ahead,
			  int behind, const struct object_id *ancestor, int total_cost, int debug)
{
	const struct object_id *norm_oid1 = oid1;
	const struct object_id *norm_oid2 = oid2;
	struct strbuf cache_dir = STRBUF_INIT;
	struct strbuf cache_path = STRBUF_INIT;
	FILE *fp;
	int swapped;
	int norm_ahead, norm_behind;

	/* Only cache if BFS was expensive (visited >= 10 commits) */
	if (total_cost < 10) {
		if (debug) {
			fprintf(stderr,
				"[DEBUG] Cache: SKIP_WRITE (total_cost=%d commits visited)\n",
				total_cost);
		}
		return;
	}

	/* Normalize the commit pair */
	swapped = normalize_commit_pair(&norm_oid1, &norm_oid2);

	/* Swap ahead/behind to match normalized order */
	if (swapped) {
		norm_ahead = behind;
		norm_behind = ahead;
	} else {
		norm_ahead = ahead;
		norm_behind = behind;
	}

	/* Create cache directory if it doesn't exist */
	strbuf_addf(&cache_dir, "%s/%s", repo_get_git_dir(the_repository), CACHE_DIR_NAME);
	mkdir(cache_dir.buf, 0777); /* Ignore errors if it already exists */

	/* Build cache file path */
	get_cache_filepath(&cache_path, norm_oid1, norm_oid2);

	/* Write cache file */
	fp = fopen(cache_path.buf, "w");
	if (!fp) {
		if (debug) {
			fprintf(stderr, "[DEBUG] Cache: failed to write %s\n", cache_path.buf);
		}
		goto cleanup;
	}

	/* Write format: "ahead,behind\n" and optionally "ancestor_oid\n" if diverged */
	fprintf(fp, "%d,%d\n", norm_ahead, norm_behind);
	if (norm_ahead > 0 && norm_behind > 0) {
		fprintf(fp, "%s\n", oid_to_hex(ancestor));
	}
	fclose(fp);

	if (debug) {
		fprintf(stderr, "[DEBUG] Cache: WRITE (%s-%s) = %d,%d (total_cost=%d)\n",
			oid_to_hex(norm_oid1), oid_to_hex(norm_oid2), norm_ahead, norm_behind,
			total_cost);
	}

	/* Prune old cache files if we exceed the limit */
	prune_old_cache_files(debug);

cleanup:
	strbuf_release(&cache_dir);
	strbuf_release(&cache_path);
}
