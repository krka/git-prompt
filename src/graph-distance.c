/*
 * graph-distance.c - Bidirectional BFS for commit distance calculation
 */
#define USE_THE_REPOSITORY_VARIABLE

#include "graph-distance.h"
#include "graph-distance-cache.h"
#include "commit.h"
#include "oidmap.h"
#include "oidset.h"
#include "hex.h"
#include "revision.h"
#include "object.h"
#include "prio-queue.h"
#include <stdio.h>

#define BFS_QUEUE_SIZE 2048 /* Power of 2 for fast modulo via bitwise AND */

/*
 * BFS node tracking during graph traversal (ring buffer entry).
 */
struct bfs_node {
  struct object_id oid;
  int distance; /* Steps from the origin (start or target) */
};

/*
 * BFS state for one side of bidirectional search (stack-allocated).
 */
struct bfs_state {
  struct bfs_node queue[BFS_QUEUE_SIZE]; /* Ring buffer for this side */
  int head;                              /* Queue head index */
  int tail;                              /* Queue tail index */
  int size;                              /* Current queue size */
  int steps_remaining;                   /* Budget remaining for this side */
};

/*
 * Entry for storing distances in oidmap during interleaved bidirectional BFS.
 */
struct bfs_distance_entry {
  struct oidmap_entry entry; /* Must be first member */
  int dist_from_start;       /* Distance from start (-1 if not reached) */
  int dist_from_target;      /* Distance from target (-1 if not reached) */
};

/*
 * Helper to get or create a distance entry in the map.
 * Returns NULL if entry exists, or the newly created entry if it didn't exist.
 */
static struct bfs_distance_entry *get_or_create_entry(struct oidmap *distances,
                                                      const struct object_id *oid)
{
  struct bfs_distance_entry *entry = oidmap_get(distances, oid);
  if (entry) {
    return NULL; /* Already exists */
  }

  entry = xmalloc(sizeof(*entry));
  oidcpy(&entry->entry.oid, oid);
  entry->dist_from_start = -1;
  entry->dist_from_target = -1;
  oidmap_put(distances, entry);
  return entry;
}

struct bfs_distance_result bfs_find_distance(const struct object_id *start,
                                             const struct object_id *target, int max_steps,
                                             int debug)
{
  struct bfs_distance_result result;
  struct oidmap distances;

  /* Initialize result with null values */
  result.ahead = -1;
  result.behind = -1;
  result.commits_visited = 0;
  oidcpy(&result.ancestor, null_oid(the_repository->hash_algo));

  /* Two BFS states: [0]=start side, [1]=target side (stack-allocated) */
  struct bfs_state states[2] = {{.head = 0, .tail = 0, .size = 0, .steps_remaining = max_steps},
                                {.head = 0, .tail = 0, .size = 0, .steps_remaining = max_steps}};

  const struct object_id *initial_oids[2];
  int commits_visited = 0;
  int side;

  /* Quick check: start == target */
  if (oideq(start, target)) {
    result.ahead = 0;
    result.behind = 0;
    result.commits_visited = 0;
    oidcpy(&result.ancestor, start); /* When same, ancestor is the commit itself */
    return result;
  }

  /* Try to get result from cache first */
  struct cache_result cached = read_distance_cache(start, target, debug);
  if (cached.found) {
    result.ahead = cached.ahead;
    result.behind = cached.behind;
    result.commits_visited = 0; /* Cache hit - no traversal needed */
    oidcpy(&result.ancestor, &cached.ancestor);
    return result;
  }

  /* Initialize distance map */
  oidmap_init(&distances, 0);

  if (debug) {
    fprintf(stderr, "[DEBUG] BFS: two-queue interleaved search...\n");
  }

  /* Setup initial state for both sides */
  initial_oids[0] = start;
  initial_oids[1] = target;

  /* Enqueue initial nodes */
  for (side = 0; side < 2; side++) {
    struct bfs_state *state = &states[side];
    struct bfs_distance_entry *entry = get_or_create_entry(&distances, initial_oids[side]);
    if (entry) {
      if (side == 0) {
        entry->dist_from_start = 0;
      } else {
        entry->dist_from_target = 0;
      }

      oidcpy(&state->queue[state->tail].oid, initial_oids[side]);
      state->queue[state->tail].distance = 0;
      state->tail = (state->tail + 1) & (BFS_QUEUE_SIZE - 1);
      state->size++;
    }
  }

  /* Interleaved BFS - alternate between queues */
  int made_progress = 1;
  while (made_progress) {
    made_progress = 0;

    for (side = 0; side < 2; side++) {
      struct bfs_state *state = &states[side];

      if (state->size <= 0 || state->steps_remaining <= 0) {
        continue;
      }

      made_progress = 1;

      /* Dequeue from this side */
      struct bfs_node current = state->queue[state->head];
      state->head = (state->head + 1) & (BFS_QUEUE_SIZE - 1);
      state->size--;
      commits_visited++;

      /* Check if we've found the intersection */
      const struct bfs_distance_entry *current_entry = oidmap_get(&distances, &current.oid);
      if (current_entry && current_entry->dist_from_start >= 0 &&
          current_entry->dist_from_target >= 0) {
        /* Found merge-base! */
        result.ahead = current_entry->dist_from_start;
        result.behind = current_entry->dist_from_target;
        result.commits_visited = commits_visited;
        oidcpy(&result.ancestor, &current.oid);
        if (debug) {
          fprintf(stderr,
                  "[DEBUG] BFS: found intersection after %d commits, "
                  "ahead=%d, behind=%d\n",
                  commits_visited, result.ahead, result.behind);
        }
        goto cleanup;
      }

      /* Parse commit and traverse parents */
      struct commit *commit = lookup_commit(the_repository, &current.oid);
      if (commit && !repo_parse_commit(the_repository, commit)) {
        struct commit_list *parent = commit->parents;
        while (parent) {
          const struct object_id *parent_oid = &parent->item->object.oid;
          struct bfs_distance_entry *parent_entry = oidmap_get(&distances, parent_oid);
          int parent_dist = current.distance + 1;

          if (!parent_entry) {
            parent_entry = get_or_create_entry(&distances, parent_oid);
            if (!parent_entry) {
              parent = parent->next;
              continue;
            }
          }

          /* Update distance for this side */
          int *dist_field =
            (side == 0) ? &parent_entry->dist_from_start : &parent_entry->dist_from_target;
          const int *other_dist_field =
            (side == 0) ? &parent_entry->dist_from_target : &parent_entry->dist_from_start;

          if (*dist_field < 0) {
            *dist_field = parent_dist;

            /* Check if we've found intersection */
            if (*other_dist_field >= 0) {
              result.ahead = parent_entry->dist_from_start;
              result.behind = parent_entry->dist_from_target;
              result.commits_visited = commits_visited;
              oidcpy(&result.ancestor, parent_oid);
              if (debug) {
                fprintf(stderr,
                        "[DEBUG] BFS: found "
                        "intersection (fast) after "
                        "%d commits, ahead=%d, "
                        "behind=%d\n",
                        commits_visited, result.ahead, result.behind);
              }
              goto cleanup;
            }

            /* Enqueue for further exploration if budget allows */
            if (state->steps_remaining > 0) {
              if (state->size >= BFS_QUEUE_SIZE - 1) {
                goto cleanup;
              }
              oidcpy(&state->queue[state->tail].oid, parent_oid);
              state->queue[state->tail].distance = parent_dist;
              state->tail = (state->tail + 1) & (BFS_QUEUE_SIZE - 1);
              state->size++;
              state->steps_remaining--;
            }
          }

          parent = parent->next;
        }
      }
    }
  }

cleanup:
  /* Set commits_visited even when we don't find merge-base (for caching decision) */
  if (result.commits_visited == 0) {
    result.commits_visited = commits_visited;
  }

  if (debug && result.ahead < 0) {
    fprintf(stderr,
            "[DEBUG] BFS: exhausted after %d commits (start steps left: %d, target "
            "steps left: %d)\n",
            commits_visited, states[0].steps_remaining, states[1].steps_remaining);
  }

  /* Write result to cache (write function decides if expensive enough) */
  write_distance_cache(start, target, result.ahead, result.behind, &result.ancestor,
                       commits_visited, debug);

  /* Free hashmap entries */
  struct oidmap_iter iter;
  struct bfs_distance_entry *entry;
  oidmap_iter_init(&distances, &iter);
  while ((entry = oidmap_iter_next(&iter))) {
    free(entry);
  }
  oidmap_clear(&distances, 0);

  return result;
}

/* Phase 2: timestamp safety margin. The true LCA is missed only if a
 * timestamp inversion on the traversed path exceeds this value. */
#define BARRIER_MARGIN 3600

#define LCA_SIDE_A (1u << 0)
#define LCA_SIDE_B (1u << 1)

struct lca_map_entry {
  struct oidmap_entry entry; /* Must be first */
  unsigned int flags;
  int dist_a; /* shortest known distance from side A (-1 if not reached) */
  int dist_b; /* shortest known distance from side B (-1 if not reached) */
};

static struct lca_map_entry *lca_get_or_create(struct oidmap *map,
                                               const struct object_id *oid)
{
  struct lca_map_entry *e = oidmap_get(map, oid);
  if (!e) {
    CALLOC_ARRAY(e, 1);
    oidcpy(&e->entry.oid, oid);
    e->dist_a = -1;
    e->dist_b = -1;
    oidmap_put(map, e);
  }
  return e;
}

struct bfs_distance_result bfs_find_merge_base(const struct object_id *start,
                                               const struct object_id *target, int max_steps,
                                               int debug)
{
  struct bfs_distance_result result;
  result.ahead = -1;
  result.behind = -1;
  result.commits_visited = 0;
  oidcpy(&result.ancestor, null_oid(the_repository->hash_algo));

  if (oideq(start, target)) {
    result.ahead = 0;
    result.behind = 0;
    oidcpy(&result.ancestor, start);
    return result;
  }

  /* Phase 1: Fast BFS for approximate common ancestor */
  struct bfs_distance_result approx = bfs_find_distance(start, target, max_steps, debug);

  if (approx.ahead < 0 || approx.behind < 0)
    return approx;

  struct commit *approx_commit = lookup_commit(the_repository, &approx.ancestor);
  if (!approx_commit || repo_parse_commit(the_repository, approx_commit))
    return approx;

  /* Phase 2 won't explore commits older than this. See BARRIER_MARGIN. */
  timestamp_t barrier = approx_commit->date - BARRIER_MARGIN;

  if (debug)
    fprintf(stderr,
            "[DEBUG] Phase 1 BFS: ancestor=%s, date=%" PRItime
            ", barrier=%" PRItime ", visited=%d\n",
            oid_to_hex(&approx.ancestor), approx_commit->date, barrier,
            approx.commits_visited);

  /* Phase 2: Timestamp-bounded LCA search */
  struct prio_queue queue = {compare_commits_by_commit_date};
  struct oidmap flags_map;
  oidmap_init(&flags_map, 0);
  int visited = 0;
  int budget = max_steps * 20;
  struct commit *best = NULL;

  struct commit *c_start = lookup_commit(the_repository, start);
  struct commit *c_target = lookup_commit(the_repository, target);
  if (!c_start || repo_parse_commit(the_repository, c_start) || !c_target ||
      repo_parse_commit(the_repository, c_target))
    goto phase2_done;

  {
    struct lca_map_entry *ea = lca_get_or_create(&flags_map, start);
    ea->flags = LCA_SIDE_A;
    ea->dist_a = 0;
    struct lca_map_entry *eb = lca_get_or_create(&flags_map, target);
    eb->flags = LCA_SIDE_B;
    eb->dist_b = 0;
  }
  prio_queue_put(&queue, c_start);
  prio_queue_put(&queue, c_target);

  while (queue.nr > 0 && visited < budget) {
    struct commit *c = prio_queue_get(&queue);
    visited++;

    if (c->date < barrier)
      break;

    if (best && c->date < best->date)
      break;

    struct lca_map_entry *e = oidmap_get(&flags_map, &c->object.oid);
    if (!e)
      continue;

    unsigned int my_flags = e->flags;

    if ((my_flags & (LCA_SIDE_A | LCA_SIDE_B)) == (LCA_SIDE_A | LCA_SIDE_B)) {
      if (!best || c->date > best->date) {
        best = c;
        result.ahead = e->dist_a;
        result.behind = e->dist_b;
      }
      continue;
    }

    struct commit_list *parent;
    for (parent = c->parents; parent; parent = parent->next) {
      struct commit *p = parent->item;
      if (repo_parse_commit(the_repository, p))
        continue;

      struct lca_map_entry *pe = lca_get_or_create(&flags_map, &p->object.oid);
      unsigned int old_flags = pe->flags;
      pe->flags |= my_flags;

      if ((my_flags & LCA_SIDE_A) && (pe->dist_a < 0 || e->dist_a + 1 < pe->dist_a))
        pe->dist_a = e->dist_a + 1;
      if ((my_flags & LCA_SIDE_B) && (pe->dist_b < 0 || e->dist_b + 1 < pe->dist_b))
        pe->dist_b = e->dist_b + 1;

      if (pe->flags != old_flags)
        prio_queue_put(&queue, p);
    }
  }

phase2_done:
  if (debug)
    fprintf(stderr, "[DEBUG] Phase 2 LCA: visited=%d, best=%s, ahead=%d, behind=%d\n",
            visited, best ? oid_to_hex(&best->object.oid) : "(none, using Phase 1)",
            result.ahead, result.behind);

  result.commits_visited = approx.commits_visited + visited;

  if (best) {
    oidcpy(&result.ancestor, &best->object.oid);
    /* ahead/behind already set when best was found */
  } else {
    result = approx;
    result.commits_visited = approx.commits_visited + visited;
  }

  clear_prio_queue(&queue);
  {
    struct oidmap_iter iter;
    struct lca_map_entry *entry;
    oidmap_iter_init(&flags_map, &iter);
    while ((entry = oidmap_iter_next(&iter)))
      free(entry);
    oidmap_clear(&flags_map, 0);
  }

  return result;
}