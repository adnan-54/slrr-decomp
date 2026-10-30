---
name: re-orchestrate
description: Orchestrate the SLRR reverse-engineering rebuild with sub-agents (.claude/agents/), splitting work between Claude (RE, task cards, review) and DeepSeek V4 Flash (implementing cards) in one session through the model router. Also a DeepSeek-only mode for when Claude limits are hit. Use at the start of every session in this repo, and whenever the user says "continue", "next round", "spawn agents", "run deepseek", "review the deepseek work", or "deepseek only".
---

# RE orchestration: Claude for accuracy, DeepSeek Flash for volume

The main session is the **orchestrator**. It does not do bulk work itself; it reads
state, picks work, spawns sub-agents, checks their results against the exe, merges,
and updates `CHECKPOINT.md`. Every agent's rules live in its file in `.claude/agents/`
(CLAUDE.md Tests rule included); the orchestrator only passes the varying input.

## 0. Mode (both run in one session)

Claude Code talks to the model router (`ANTHROPIC_BASE_URL`, user settings), which serves
Claude models and `router-claude/deepseek-v4-flash[1m]`. Agents pin their own model, so one
session mixes Claude and Flash. No separate DeepSeek settings file.

- **Mixed (default)**: main session on Claude (opus). Agents per the §2 table.
- **DeepSeek-only** (Claude limits hit; user says "deepseek only" or `/re-orchestrate deepseek-only`):
  switch the main session with `/model` → "DeepSeek V4 Flash". Spawn only `card-implementer`,
  `ds-tracer` and `ds-reviewer` (never pass `model:` to the Agent tool; never spawn the Claude agents).
  Follow §2c.

## 1. Start of session

1. Read `CHECKPOINT.md` (state, next steps) and `tasks/` (card statuses).
2. List unmerged branches and worktrees: `git branch --list 'card/*' 'worktree-agent-*'`, `git worktree list`.
3. Mixed mode: `grep -l '^review: deepseek' tasks/*.md` lists cards merged in DeepSeek-only mode.
   Re-review them first (`card-reviewer`; `model: opus` for core logic), then set `review: claude`.
4. Decide the work for this session; say it in one line.

## 2. Roles → agents (mixed mode)

| Work | Agent | Model / effort | Notes |
|---|---|---|---|
| Trace exe, write `notes/`, decide formulas, split into cards | `re-tracer` | opus / medium | rea here; orchestrator commits notes + cards |
| Implement a card | `card-implementer` | Flash / high | own worktree, commits `card/<id>`; input = card path only |
| Review a branch vs exe, routine | `card-reviewer` | sonnet / medium | read-only, no build; samples cited addresses |
| Review core logic (physics, VM, vehicle) | `card-reviewer` + `model: opus` | opus / medium | decompiles every cited address |
| Small fix from review (< ~30 lines) | `card-fixer` (`model: opus` after 2 failed fixes) | sonnet / medium | commits on the card branch |
| Mechanical side jobs (grep/list/rename, names.json) | `mech-helper` | haiku / low | |
| Large rework found in review | → new card | — | back through `card-implementer` |

Ladder: **cards = opus medium; implementation = Flash; verification = sonnet, re-verify with opus
only if it finds issues; fix = sonnet, opus only after 2 failed fixes.** Opus never above `medium`.
Never use DeepSeek Pro. Never let Flash agents explore the exe open-endedly with rea.

## 2a. Round flow (parallel)

1. Spawn one `card-implementer` per independent card at once (`run_in_background`); cards that
   touch the same file go one after another. Meanwhile run `re-tracer` for the next cards.
2. Per finished card, at the same time: `tools/card-gate.sh check <id>` (Bash, background) and
   the reviewer (`card-reviewer`, or `ds-reviewer` in DeepSeek-only mode).
3. Both pass → `tools/card-gate.sh merge <id>` (main checkout, serialized by a lock), set
   `status: merged` and `review: claude|deepseek-pass` in the card, commit, delete `card/<id>`.
   FAIL → `card-fixer` (mixed) or a follow-up card; red check → read `$LOGDIR/card-check-<id>.txt`.
4. Commit notes/cards on main between merges. Commit only from the main checkout.

## 2b. Agent communication rules (keep the orchestrator context small)
- **Send paths, not summaries.** Pass the card path, note path or branch name, never a paraphrase:
  summaries drift and cost orchestrator tokens.
- **Agents return small reports** (<= ~15 lines; each agent file says so). Long detail goes in the
  card/notes file.

## 2c. DeepSeek-only ladder (Claude limits hit)

Flash is less exact, so every step is bounded and leaves a trail for Claude.

| Work | How |
|---|---|
| Pick next targets | orchestrator reads the CHECKPOINT next-steps queue + notes; rea only for bounded lookups (callees/xrefs of a named address) |
| Trace + card | `ds-tracer` with `<id>: <addr Class.method(sig)>; ...` → `status: draft` card. Promote to `todo` only without `UNRESOLVED` and within §3 limits; else split or skip |
| Implement | `card-implementer`, §2a flow |
| Review | `ds-reviewer` (decompiles every cited address) → `review: deepseek-pass/fail` |
| Fix | FAIL → follow-up card; 2 failed fixes → `status: blocked`, left for Claude |
| Mechanical jobs, notes/, CHECKPOINT | orchestrator inline |

- Core logic is allowed but always gets the Claude re-review (§1 step 3).
- Cost: rea decompile output is the big token sink. Keep a round to ≤ ~5 cards, then end the session.
- CHECKPOINT "State": name the card range done in DeepSeek-only mode.

## 3. Task cards (`tasks/NNN-slug.md`) — sized for Flash's small context

Cards must let a Flash agent finish **without rea and without reading large files**:
≤ ~5 exe functions, ≤ 3 source files to touch, all exe evidence pasted in. Split larger work.

```markdown
---
id: 042
status: todo            # draft | todo | implemented | merged | blocked | rejected
branch:                 # filled by the implementing agent
review:                 # claude | deepseek-pass | deepseek-fail (set at merge)
depends: [041]
---
# <subsystem>: <what>

## Goal
One paragraph: what behaviour to port, which natives/functions become non-stubs.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x4b6420 WheelRef.setFriction
<pseudocode, with struct offsets mapped to our field names>

## Files
- edit: src/game/car.cpp (WheelData::set_friction), src/script/natives.cpp (registry row)
- read only: src/game/car.h (struct)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
```

## 3b. DeepSeek cache hits (misses cost much more)

DeepSeek caches by exact prompt prefix.
- **The frozen prompt is the agent file.** Don't edit `card-implementer.md`, `ds-tracer.md` or
  `ds-reviewer.md` mid-session. The Agent prompt is only the varying input (card path, or id + addresses):
  no dates, counts or round commentary.
- **Shared reads first, card last**: CLAUDE.md, then the card, then the listed files.
- **Parallel worktrees vary the working directory** in each agent's system prompt, so the cache
  hit rate drops a bit. Accepted for speed; the large tool/system prefix before it still hits.
- **Keep agents short**: one small card per agent so context never gets compacted.
- **Check the hit rate** in the DeepSeek usage dashboard; if low, look for a varying prefix first.

## 3c. Build less, wait less

`make` runs `-j$(nproc)` by default and every check line is its own target, so they run side by side.
- Never `make clean` per step: `-MMD` deps make incremental builds safe. `make -C src clean check`
  only once at session end (§5).
- `make -C src check-fast` = every check except `--race`/`--city`: agents use it while iterating,
  `make -C src check` once before commit.
- `tools/card-gate.sh check` re-runs the check in the card's own worktree (incremental, already built).
- Run long checks with `run_in_background` and do the next review/trace/card meanwhile.

## 4. Review gate

Reviewers never build: `card-gate.sh check` is the build gate. For each `implemented` card:
1. Reviewer agent reads `git diff main...card/<id> -- src` and decompiles the cited addresses
   (all for core logic, a sample otherwise); compares constants, conditions, order, edge cases.
2. It flags uncited logic not marked `INVENTED:` and any new test code (reject it, per `CLAUDE.md`).
3. PASS + green check → `card-gate.sh merge <id>`. A non-fast-forward merge re-checks main itself.
4. Report to the user: what merged, what was wrong, corrections to earlier claims.

## 5. End of session

- Run `make -C src clean check` once (full gate).
- `git worktree prune`; remove leftover `card/*` worktrees of merged cards.
- Update `CHECKPOINT.md` "State" and "Known open spots"; commit; push only if the user asked.
- Tell the user: which cards are queued, rough size, and whether DeepSeek-only mode was used.
