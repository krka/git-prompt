#!/bin/bash
#
# Integration test for the merge-base subcommand.
# Creates repos with various topologies and verifies that
# git-prompt merge-base matches git merge-base.
#
set -e

PROMPT="$(cd "$(dirname "$0")/.." && pwd)/target/git-prompt"
PASS=0
FAIL=0
TOTAL=0

check() {
  local desc="$1" from="$2" to="$3"
  TOTAL=$((TOTAL + 1))

  local expected actual
  expected=$(git merge-base "$from" "$to" 2>/dev/null) || expected="NONE"
  actual=$("$PROMPT" merge-base --from="$from" --to="$to" --max-traversal=5000 --local 2>/dev/null) || actual="NONE"

  if [ "$expected" = "$actual" ]; then
    echo "  PASS  $desc"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $desc"
    echo "        expected: $expected"
    echo "        actual:   $actual"
    FAIL=$((FAIL + 1))
  fi
}

tmpdir=$(mktemp -d /tmp/merge-base-test-XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

# ── Test 1: Linear history (squash-merge style) ──────────────────────
echo "=== Linear history ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
for i in $(seq 1 10); do
  echo "$i" > f.txt && git add f.txt && git commit -q -m "commit $i"
done

check "HEAD vs HEAD~5"   HEAD   HEAD~5
check "HEAD vs HEAD~1"   HEAD   HEAD~1
check "HEAD~3 vs HEAD~7" HEAD~3 HEAD~7
check "HEAD vs HEAD"     HEAD   HEAD

# ── Test 2: Simple branch (single merge-base) ────────────────────────
echo ""
echo "=== Simple branch ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
echo "base" > f.txt && git add f.txt && git commit -q -m "base"
# fork point
echo "v2" > f.txt && git add f.txt && git commit -q -m "master-2"
git checkout -q -b feature HEAD~1
echo "feat" > g.txt && git add g.txt && git commit -q -m "feature-1"
echo "feat2" > g.txt && git add g.txt && git commit -q -m "feature-2"

check "master vs feature" master feature
check "feature vs master" feature master

# ── Test 3: Merge-commit topology (like monorepo) ────────────────────
echo ""
echo "=== Merge-commit topology ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
echo "base" > f.txt && git add f.txt && git commit -q -m "base"

# Create a series of merge-commits: master merges a branch at each step
for i in $(seq 1 5); do
  # Create a branch with 3 commits
  git checkout -q -b "branch-$i"
  for j in 1 2 3; do
    echo "b${i}-${j}" > "b${i}.txt" && git add "b${i}.txt" && git commit -q -m "branch-$i commit $j"
  done
  # Merge back to master
  git checkout -q master
  echo "m$i" >> f.txt && git add f.txt && git commit -q -m "master-$i"
  git merge -q --no-edit "branch-$i"
done

# Now test merge-base between various points
check "HEAD vs HEAD~2"           HEAD          HEAD~2
check "HEAD vs branch-1"         HEAD          branch-1
check "branch-5 vs branch-1"    branch-5      branch-1
check "branch-3 vs branch-4"    branch-3      branch-4

# ── Test 4: Diamond merge (multiple paths to common ancestor) ────────
echo ""
echo "=== Diamond merge ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
echo "root" > f.txt && git add f.txt && git commit -q -m "root"
# Two branches from root
git checkout -q -b left
echo "left" > l.txt && git add l.txt && git commit -q -m "left-1"
git checkout -q master
git checkout -q -b right
echo "right" > r.txt && git add r.txt && git commit -q -m "right-1"
# Merge both into master
git checkout -q master
git merge -q --no-edit left
git merge -q --no-edit right

check "HEAD vs left"  HEAD left
check "HEAD vs right" HEAD right
check "left vs right" left right

# ── Test 5: Deep merge-commit DAG (wormhole stress test) ─────────────
echo ""
echo "=== Deep merge DAG (20 merges, 5 branch commits each) ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
echo "base" > f.txt && git add f.txt && git commit -q -m "base"

for i in $(seq 1 20); do
  git checkout -q -b "deep-$i"
  for j in $(seq 1 5); do
    echo "d${i}-${j}" > "d${i}.txt" && git add "d${i}.txt" && git commit -q -m "deep-$i-$j"
  done
  git checkout -q master
  echo "m$i" >> f.txt && git add f.txt && git commit -q -m "master-$i"
  git merge -q --no-edit "deep-$i"
done

check "HEAD vs HEAD~10"         HEAD          HEAD~10
check "HEAD vs HEAD~30"         HEAD          HEAD~30
check "HEAD vs deep-1"          HEAD          deep-1
check "HEAD vs deep-10"         HEAD          deep-10
check "deep-5 vs deep-15"       deep-5        deep-15

# ── Test 6: Branch that merged master into it ─────────────────────────
echo ""
echo "=== Branch with merge-from-master ==="
cd "$tmpdir" && rm -rf repo && git init repo -q && cd repo
git config user.name "Test" && git config user.email "t@t.com"
echo "base" > f.txt && git add f.txt && git commit -q -m "base"
echo "v2" > f.txt && git add f.txt && git commit -q -m "master-2"
git checkout -q -b feature HEAD~1
echo "feat" > g.txt && git add g.txt && git commit -q -m "feature-1"
# Merge master into feature
git merge -q --no-edit master
echo "feat2" > g.txt && git add g.txt && git commit -q -m "feature-2"
# Add more commits on master
git checkout -q master
echo "v3" > f.txt && git add f.txt && git commit -q -m "master-3"

check "master vs feature" master feature

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "============================================"
if [ "$FAIL" -eq 0 ]; then
  echo "ALL PASSED: $PASS/$TOTAL"
else
  echo "FAILED: $FAIL/$TOTAL ($PASS passed)"
  exit 1
fi
