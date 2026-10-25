---
name: reference-checkpoint
description: Where the resume/checkpoint state of the SLRR rebuild lives and how to update it
metadata:
  type: reference
---

`/root/slrr-re/CHECKPOINT.md` holds the goal, decisions, environment, repo map, current state and ordered next steps; `tools/checkpoint.sh` verifies the setup. The user asked for this so work can resume in a new session or agent.

**How to apply:** at every stop/milestone, update the "State" and "Next-steps queue" sections of CHECKPOINT.md and commit it. Related: [[feedback-generic-naming]].
