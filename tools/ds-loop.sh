#!/bin/bash
# sequential DeepSeek card runner: run, verify commit, check, ff-merge into main
S=$(dirname "$0"); cd /root/slrr-re
for id in "$@"; do
  card=$(ls tasks/${id}-*.md)
  echo "=== $id $card $(date +%T)"
  "$S/ds-card.sh" "$card" > "${LOGDIR:-/tmp}/ds-log-$id.txt" 2>&1
  # agent's own commit is often blocked by the permission gate; commit leftovers here
  if [ "$(git branch --show-current)" = "card/$id" ] && [ -n "$(git status --porcelain)" ]; then
    git add -A src tasks && git commit -qm "card $id: $(sed -n 's/^# //p' "$card" | head -1)

Co-Authored-By: DeepSeek V4 Flash <noreply@deepseek.com>"; fi
  git switch -q main 2>/dev/null
  # agents sometimes name the branch card/<id>-<slug>; normalize it
  alt=$(git branch --format='%(refname:short)' --list "card/$id-*" | head -1)
  if ! git rev-parse -q --verify card/$id >/dev/null && [ -n "$alt" ]; then
    git branch -m "$alt" card/$id; fi
  if ! git rev-parse -q --verify card/$id >/dev/null || [ "$(git rev-list --count main..card/$id)" = 0 ]; then
    echo "FAIL $id: no commit on card/$id"; git status --short | head; exit 1; fi
  if git show card/$id:"$card" | grep -q "^status: blocked"; then echo "BLOCKED $id: card/$id kept unmerged (agent hit its stop rule)"; exit 1; fi
  # check on the card branch first: a red card must never land on main
  git switch -q card/$id
  if ! make -C src clean check > "${LOGDIR:-/tmp}/ds-check-$id.txt" 2>&1; then
    git switch -q main; echo "FAIL $id: make check (card/$id kept unmerged)"; tail -20 "${LOGDIR:-/tmp}/ds-check-$id.txt"; exit 1; fi
  git switch -q main
  if ! git merge -q --ff-only card/$id && ! git merge -q --no-edit card/$id; then echo "FAIL $id: merge"; exit 1; fi
  echo "OK $id"
done
