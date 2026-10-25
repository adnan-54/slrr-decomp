---
name: re-orchestrate
description: Orchestrate the SLRR reverse-engineering rebuild with sub-agents, splitting work between Claude (RE, task cards, review) and DeepSeek Flash (implementing cards). Use at the start of every session in this repo, and whenever the user says "continue", "next round", "spawn agents", "run deepseek", or "review the deepseek work".
---

# RE orchestration: tiered models, cheap orchestrator

The main session only routes: read state, pick work, spawn agents, merge, update
`CHECKPOINT.md`. It does no bulk work. Run it on **Sonnet** (`/model sonnet`).
Follow `CLAUDE.md` (Tests rule!) in every agent prompt.

## Stages and models

| Stage | Who | Model |
|---|---|---|
| Orchestration | main session | sonnet |
| RE + card creation | agent `re-analyst` | opus (medium effort, never higher) |
| Implementation | DeepSeek session (§3) | deepseek-v4-flash |
| Verification | agent `card-reviewer` | sonnet; re-review with `model: opus` only on FAIL/`ESCALATE OPUS` |
| Fix | agent `card-fixer` (or DeepSeek card if big) | sonnet; `model: opus` only after 2 failed fixes |
| Mechanical side jobs (grep/list/rename) | `Agent` with `model: haiku` | haiku |

Agent definitions: `.claude/agents/`. Never use DeepSeek Pro. Never let DeepSeek explore
the exe with rea (tokens + invented behaviour).

## Orchestrator rules (these save the most tokens)
- Pass **file paths** (card path, branch), never a paraphrased summary of a card.
- Every agent returns a **short structured report** (<= 15 lines: `PASS/FAIL` + bullets, or card paths + addresses). Do not paste long reports back into the context.
- Fresh agent per task. Independent cards in parallel only when they touch different files.
- Code-editing Claude agents: `isolation: "worktree"`, first `ln -s /root/slrr-re/bin bin` (bin/ is gitignored, never commit), commit on branch, do not push.

## 1. Start of session (≈3 tool calls)
1. Read `CHECKPOINT.md` only. Notes are read on demand through `notes/INDEX.md`; `notes/LOG.md` is history (grep, never read whole).
2. `ls tasks/` (open cards only; merged ones are in `tasks/done/`) and `git branch --list 'card/*' --no-merged main`.
3. Detect the provider: `echo "${ANTHROPIC_BASE_URL:-anthropic}"`. `api.deepseek.com` = DeepSeek session (only implements queued cards, `claude --settings .claude/settings-deepseek.json`); otherwise Claude session (RE, cards, review, merge).
4. Say the plan in one line, then dispatch.

A round: `Claude: re-analyst writes cards` → `DeepSeek: implement` → `Claude: card-reviewer, merge, next cards`.

## 2. Cards
Format and the frozen DeepSeek prompt: `tasks/README.md`. A card must fit Flash: <= ~5 exe functions, <= 3 files, all evidence pasted in. On merge: set `status: merged` and `git mv` it to `tasks/done/`.

## 3. DeepSeek session: cache hits (misses cost much more)
- Use the frozen prompt in `tasks/README.md` verbatim; only the last line (card path) varies. No dates, counts or commentary in it.
- Cards one after another in the main checkout (a different worktree path per agent breaks the shared prefix). Agent makes `card/NNN` from `main`, commits, switches back.
- One small card per agent so context never compacts. Too big → stop and split.
- Don't change model/effort/settings mid-session; keep the DeepSeek orchestrator short: read queue, dispatch, report, end. Scripts: `tools/ds-card.sh`, `tools/ds-loop.sh`.
- After a phase check the cache hit rate in the DeepSeek dashboard; if low, look for a varying prefix first.

## 4. Review gate (Claude session)
For each branch with `status: implemented`:
1. Spawn `card-reviewer` with card path + branch (sonnet).
2. `PASS` → `git merge --no-edit`, (no per-card build; one `make -C src clean check` per batch at the end), status merged, move card to `done/`, delete branch/worktree.
3. `FAIL` → small → `card-fixer`; after 2 failures or `ESCALATE OPUS` → same agent with `model: opus`; large → new card for the next DeepSeek phase.
4. Tell the user in a few lines: what merged, what was wrong, corrections to earlier claims.

## 5. End of session
- Update `CHECKPOINT.md` "State" + "Latest session" (keep it ≈10 KB: move older entries to the top of `notes/LOG.md`); commit; push only if asked.
- Tell the user the next mode (Claude or DeepSeek), queued cards, rough size.
