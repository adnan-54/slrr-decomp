#!/bin/bash
# sequential DeepSeek card runner: run, verify commit, check, ff-merge into main
S=$(dirname "$0"); cd /root/slrr-re
for id in "$@"; do
  card=$(ls tasks/${id}-*.md)
  echo "=== $id $card $(date +%T)"
  "$S/ds-card.sh" "$card" > "$S/log-$id.txt" 2>&1
  # agent's own commit is often blocked by the permission gate; commit leftovers here
  if [ "$(git branch --show-current)" = "card/$id" ] && [ -n "$(git status --porcelain)" ]; then
    git add -A src tasks && git commit -qm "card $id: $(sed -n 's/^# //p' "$card" | head -1)

Co-Authored-By: DeepSeek V4 Flash <noreply@deepseek.com>"; fi
  git switch -q main 2>/dev/null
  if ! git rev-parse -q --verify card/$id >/dev/null || [ "$(git rev-list --count main..card/$id)" = 0 ]; then
    echo "FAIL $id: no commit on card/$id"; git status --short | head; exit 1; fi
  if ! git merge -q --ff-only card/$id && ! git merge -q --no-edit card/$id; then echo "FAIL $id: merge"; exit 1; fi
  if ! make -C src clean check > "$S/check-$id.txt" 2>&1; then echo "FAIL $id: make check"; tail -20 "$S/check-$id.txt"; exit 1; fi
  echo "OK $id"
done
