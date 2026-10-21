#!/bin/bash
# run one card with DeepSeek Flash headless; frozen prompt per re-orchestrate §3b
cd /root/slrr-re
card="$1"
prompt="You implement one task card in this repo. Read CLAUDE.md, then the card, then only
the files the card lists, in order. Do not use rea/Ghidra tools; all exe evidence is
in the card. Steps: git switch -c card/<id> main; implement; make -C src clean check;
set the card's status to implemented and branch to card/<id>; commit (message ends
with \"Co-Authored-By: DeepSeek V4 Flash <noreply@deepseek.com>\"); git switch main. Do not push.
Report: branch, commit, what you changed, anything marked INVENTED, doubts.
Card: $card"
claude -p "$prompt" --settings .claude/settings-deepseek.json \
  --permission-mode acceptEdits \
  --allowedTools "Read,Edit,Write,Glob,Grep,Bash(cd *),Bash(git *),Bash(make *),Bash(ls *),Bash(cat *),Bash(grep *),Bash(sed -n *),Bash(head *),Bash(tail *),Bash(wc *),Bash(find *),Bash(diff *),Bash(rtk git *),Bash(rtk make *)" \
  --disallowedTools "mcp__rea,Bash(git push *)" \
  --max-turns 80 2>&1
