---
name: re-orchestrate
description: Orchestrate the SLRR reverse-engineering rebuild with sub-agents, splitting work between Claude (RE, task cards, review) and DeepSeek Flash (implementing cards). Use at the start of every session in this repo, and whenever the user says "continue", "next round", "spawn agents", "run deepseek", or "review the deepseek work".
---

# RE orchestration: Claude for accuracy, DeepSeek Flash for volume

The main session is the **orchestrator**. It does not do bulk work itself; it reads
state, picks work, spawns sub-agents, checks their results against the exe, merges,
and updates `CHECKPOINT.md`. Follow `CLAUDE.md` (Tests rule!) in every agent prompt.

## 0. Detect the mode (one session = one provider)

Claude Code talks to one API endpoint per session, so Claude and DeepSeek cannot run
side by side. Check which one this session is:

```bash
echo "${ANTHROPIC_BASE_URL:-anthropic}"   # api.deepseek.com => DeepSeek mode
```

- **Claude mode** (Claude plan, flat cost up to usage limits): RE, task cards, review, merge.
- **DeepSeek mode** (`claude --settings .claude/settings2.json`, pay per token): only implement queued cards.

The provider changes per *phase*, so a round is:
`Claude: RE + cards` → `DeepSeek: implement cards` → `Claude: review + merge + next cards`.
End every session by telling the user which mode the next session should use and why.

## 1. Start of session (both modes)

1. Read `CHECKPOINT.md` (state, next steps) and `tasks/` (card statuses).
2. List unmerged agent branches: `git branch --list 'card/*' 'worktree-agent-*'`.
3. Decide the work for this session from the mode table below; say it in one line.

## 2. Roles, models, effort

| Work | Mode | Agent model | Effort | Notes |
|---|---|---|---|---|
| Trace exe functions, write `notes/`, decide formulas | Claude | opus | high | rea/Ghidra here only; huge decompile output is free on the plan |
| Split a traced subsystem into task cards (`tasks/`) | Claude (orchestrator or opus agent) | opus | medium | paste the needed pseudocode into the card |
| Implement a card | DeepSeek | deepseek-v4-flash (any alias maps to it) | high | one card per agent, worktree isolation |
| Review a DeepSeek branch vs exe, routine | Claude | sonnet | medium | sample-decompile the cited addresses |
| Review hard/core logic (physics, VM, vehicle) | Claude | opus | high | decompile every changed function |
| Small fix found in review (< ~30 lines) | Claude | orchestrator or sonnet | low | don't spend a DeepSeek round on it |
| Large rework found in review | → new card | — | — | goes to the next DeepSeek phase |

Never use DeepSeek Pro (cost, not worth it). Never let DeepSeek agents explore the
exe open-endedly with rea: that is where tokens go and where they invent behaviour.

Spawn agents with `isolation: "worktree"`; in the prompt tell them to run
`ln -s /root/slrr-re/bin bin` first (bin/ is gitignored, never commit it), to commit
on their branch, and **not push**. Run independent cards in parallel (different files);
cards that touch the same file run one after another.

## 3. Task cards (`tasks/NNN-slug.md`) — sized for Flash's small context

Claude writes cards so a Flash agent can finish **without rea and without reading
large files**. A card must fit: ≤ ~5 exe functions, ≤ 3 source files to touch, and
all exe evidence pasted in. If a card needs more, split it.

```markdown
---
id: 042
status: todo            # todo | implemented (branch) | merged | rejected
branch:                 # filled by the implementing agent
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
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
```

## 4. Review gate (Claude mode)

For each branch with `status: implemented`:
1. `git diff main...<branch> --stat`, then read the diff.
2. Decompile the cited addresses (all of them for core logic, a sample otherwise)
   and compare branch by branch: constants, conditions, order of operations, edge cases.
3. Grep the diff for uncited logic that is not marked `INVENTED:` and for any new
   test code (reject it, per `CLAUDE.md`).
4. Merge (`git merge --no-edit`), run `make -C src clean check`, set `status: merged`,
   delete the branch/worktree. Otherwise fix small issues yourself or write a follow-up card.
5. Report to the user: what merged, what was wrong, and corrections to earlier claims.

## 5. End of session

- Update `CHECKPOINT.md` "State" and "Known open spots"; commit; push only if the user asked.
- Tell the user: next mode (Claude or DeepSeek), which cards are queued, rough size.
