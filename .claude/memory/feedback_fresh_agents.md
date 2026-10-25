---
name: feedback-fresh-agents
description: Run each task in a fresh sub-agent instead of the long orchestrator context
metadata:
  node_type: memory
  type: feedback
  originSessionId: 86986c35-62a4-460a-8aa9-d8f09c717917
  modified: 2026-10-24T04:01:20.995Z
---

Spawn a fresh agent (Agent tool, worktree isolation for code) for every task instead of doing the work in the orchestrator's growing context; the orchestrator only writes the self-contained prompt, reviews the result and merges.

**Why:** the user said (2026-10-24) "spawn a fresh agent so we dont use this big context, do this for every task" — large contexts burn plan usage.

**How to apply:** orchestrator keeps to card writing, short reviews and merges; Claude agents (opus/sonnet medium) or DeepSeek (tools/ds-loop.sh) do the work. Prompts must carry all needed context (CLAUDE.md rules, card path, branch, check command). Related: [[feedback-no-selfwritten-tests]].
