#!/bin/bash
# DeepSeek Flash headless: trace listed exe natives with rea and write a DRAFT task card.
# usage: tools/ds-trace.sh <card-id> "<addr Class.method(sig)>; ..."   (Claude reviews drafts)
cd /root/slrr-re
prompt="You trace exe functions and write one draft task card; you do not change src/.
Read CLAUDE.md, .claude/skills/re-orchestrate/SKILL.md section 3 (card format), and tasks/047-*.md as a format example.
Use the rea MCP tools: open_binary path \"/root/slrr-re/bin/the game/StreetLegal_Redline.exe\", then procedure_pseudo_code (arg procedure = address string) for each listed native and only the helpers it calls that are needed to port it (max ~5 functions). Use grep on src/ to find our existing struct fields, helpers and similar implemented natives (src/script/natives.cpp, src/game/car.*); map exe offsets to our field names only when the src comment cites the same offset.
Never guess: if an offset, constant or callee is unclear, write UNRESOLVED in the card with the raw pseudocode line. Convert hex float constants to decimal and keep the hex.
Write tasks/<id>-<slug>.md: frontmatter status: draft, branch empty, depends []; Goal; Exe evidence (cleaned pseudocode per function with address); Files (exact paths and functions; at most 3 files); Done when (per the skill). If a native needs a service that does not exist in src/, say so and specify the exe return value for that case.
Commit only that card file on main, message ending \"Co-Authored-By: DeepSeek V4 Flash <noreply@deepseek.com>\". Do not push.
Report: card path, UNRESOLVED items.
Card id and natives: $1: $2"
claude -p "$prompt" --settings .claude/settings-deepseek.json --permission-mode acceptEdits \
  --allowedTools "Read,Write,Edit,Glob,Grep,mcp__rea__open_binary,mcp__rea__procedure_pseudo_code,mcp__rea__read_function_instructions,mcp__rea__procedure_callees,Bash(git *),Bash(rtk git *),Bash(ls *),Bash(grep *),Bash(rtk grep *),Bash(cat *)" \
  --disallowedTools "Bash(git push *),Bash(rtk git push *)" --max-turns 80 2>&1
