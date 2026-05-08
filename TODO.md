# TODO

## Merge-base algorithm weaknesses

1. **Ahead/behind counts are BFS hops, not true commit counts** — for non-linear
   histories (branches with merges), hop count to the merge-base can differ
   from `git rev-list --count`. Linear histories are exact.

2. **No generation number support** — priority queue orders by commit timestamp.
   If commit-graph is available, generation numbers give topologically correct
   ordering immune to clock skew.

3. **Clock skew can produce non-optimal result** — the PQ relies on commit dates
   being monotonic (parents older than children). Large timestamp inversions can
   cause a valid but non-optimal ancestor to be returned.
