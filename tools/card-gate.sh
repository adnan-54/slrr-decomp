#!/bin/bash
# Card gate (no LLM): check and merge one card branch made by the card-implementer agent.
# usage: tools/card-gate.sh check <id>   make check in the branch's worktree -> $LOGDIR/card-check-<id>.txt
#        tools/card-gate.sh merge <id>   merge card/<id> into main (run from the main checkout), drop its worktree
# Run `check` in the background and the review agent at the same time; `merge` only when both pass.
cd "$(dirname "$0")/.." || exit 1
cmd="$1"; id="$2"; LOG=${LOGDIR:-/tmp}
[ -n "$id" ] || { echo "usage: $0 check|merge <id>"; exit 2; }
card=$(ls tasks/${id}-*.md 2>/dev/null | head -1)
[ -n "$card" ] || { echo "FAIL $id: no card"; exit 1; }

# agents sometimes name the branch card/<id>-<slug>; normalize it
alt=$(git branch --format='%(refname:short)' --list "card/$id-*" | head -1)
if ! git rev-parse -q --verify "card/$id" >/dev/null && [ -n "$alt" ]; then git branch -m "$alt" "card/$id"; fi
if ! git rev-parse -q --verify "card/$id" >/dev/null || [ "$(git rev-list --count main..card/$id)" = 0 ]; then
  echo "FAIL $id: no commit on card/$id"; exit 1; fi
if git show "card/$id:$card" | grep -q "^status: blocked"; then echo "BLOCKED $id: agent hit its stop rule"; exit 1; fi

# worktree that has card/<id> checked out (empty if none)
wt=$(git worktree list --porcelain | awk -v b="branch refs/heads/card/$id" '/^worktree /{p=substr($0,10)} $0==b{print p}')

case "$cmd" in
check)
  [ -n "$wt" ] || { echo "FAIL $id: no worktree has card/$id checked out"; exit 1; }
  [ -e "$wt/bin" ] || ln -s "$PWD/bin" "$wt/bin"
  # incremental: the agent already built there
  if make -C "$wt/src" check > "$LOG/card-check-$id.txt" 2>&1; then echo "OK $id: check green ($LOG/card-check-$id.txt)"
  else echo "FAIL $id: make check"; tail -20 "$LOG/card-check-$id.txt"; exit 1; fi ;;
merge)
  [ "$(git branch --show-current)" = main ] || { echo "FAIL $id: main checkout is not on main"; exit 1; }
  # parallel gates: one merge into main at a time
  exec 9>"$(git rev-parse --git-common-dir)/card-gate.lock"; flock 9
  if git merge -q --ff-only "card/$id"; then echo "MERGED $id (fast-forward, tree already checked)"
  elif git merge -q --no-edit "card/$id"; then
    # merged tree differs from the checked branch: check main itself
    if ! make -C src check > "$LOG/card-check-$id-main.txt" 2>&1; then
      echo "FAIL $id: make check on main after merge (merge commit kept; see $LOG/card-check-$id-main.txt)"; exit 1; fi
    echo "MERGED $id (merge commit, main check green)"
  else git merge --abort 2>/dev/null; echo "FAIL $id: merge conflict"; exit 1; fi
  [ -n "$wt" ] && git worktree remove --force "$wt" ;;
*) echo "usage: $0 check|merge <id>"; exit 2 ;;
esac
