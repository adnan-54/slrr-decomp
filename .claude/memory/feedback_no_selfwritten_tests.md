---
name: feedback-no-selfwritten-tests
description: "In this RE project, don't write unit tests with self-made expected values; only corpus checks on shipped data"
metadata:
  node_type: memory
  type: feedback
  originSessionId: be998bf4-e8f0-416a-a36d-2c59de7682f0
  modified: 2026-10-21T01:42:38.769Z
---

Don't write unit tests / assertion suites whose expected values come from my own reading of the exe. Only checks that run over shipped game data (bin/ samples, corpora) are worth keeping.

**Why:** User (2026-10-20) judged the DeepSeek round's self-written tests (audiocheck, rendercheck, inputcheck, ...) as burning tokens/time; they can't catch a misread of the exe and give false confidence (e.g. invented audio rolloff "tested").

**How to apply:** verify by citing/comparing the decompiled exe function; mark logic not derived from the exe as invented; this overrides the ponytail "leave a self-check" rule here.
