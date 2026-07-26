# Working with this user on this project

Established through the Linux-port session (2026-07-25). Applies to future sessions
on this repo.

## Verify before declaring done — this user catches inflated reports

Across this session, background-agent ("fork") reports of "done, verified" were
repeatedly found to be incomplete or outright wrong when independently checked: a
"completed" report once cut off mid-sentence with the actual build still broken; a
"verified working" render claim turned out to need a from-scratch rebuild to
confirm; a reported fix for a hang turned out to leave a second, undiscovered
deadlock. **Always independently rebuild from scratch (`rm -rf build && cmake -B
build -S . && cmake --build build`) and actually run the binary before reporting a
phase/fix as complete** — don't relay a sub-agent's or your own untested claim
verbatim. This user notices the difference and will call it out. This is confirmed
good practice, not overcaution — keep doing it.

## The user actively playtests and reports precise symptoms

They watch the live game window themselves and report exactly what they see/don't
see ("W and D swapped", "mouse was captured, but no movement", "it crashed after a
few seconds", "no jump") — treat these as precise bug reports, not vague impressions.
Take screenshots they share seriously as primary evidence (e.g., the malformed
chunk-rendering screenshot was the single most useful piece of debugging information
in the whole session — it proved real geometry was reaching the GPU, ruling out a
whole category of "nothing is rendering" hypotheses in one image).

## Comfortable with deep, long technical debugging — wants hands-on digging, not just delegation

When told "keep debugging," the user wants actual root-cause investigation (reading
source, attaching gdb to live/crashed processes, comparing backtraces across time)
— not another round of spawning research-only agents. This session's most effective
debugging moments were direct: reading exact crash backtraces via
`coredumpctl`/`gdb`, then reading the implicated source function line-by-line rather
than guessing.

## Respect interruptions immediately

The user interrupted tool calls mid-execution multiple times (to redirect, to share
a screenshot, to ask for something unrelated like this memory-persistence task).
Stop cleanly and address what they actually asked for — don't try to finish the
interrupted action first or re-litigate the prior plan.

## Wants durable knowledge in the repo itself, not just private assistant memory

Explicitly asked for `CLAUDE.md` + `.claude/` + `.memory/` to be created and kept
up to date with session learnings, rather than relying solely on cross-session
assistant memory the user can't see or edit directly. Keep these files current as
work continues — they're the source of truth for "what's known" about this port,
not a one-time snapshot. When a bug from `KNOWN_BUGS.md` gets fixed or a status in
`linux-port-plan-and-status.md` changes, update those files as part of the same
work, not as an afterthought.

## Scope discipline on the port itself

Confirmed via an explicit question-and-answer during planning: the user wants
Iggy/menu-system work deferred indefinitely in favor of getting straight to a
playable world first (see `ARCHITECTURE.md`'s Iggy section). Don't reopen that
question — it's a settled decision, not an open one.
