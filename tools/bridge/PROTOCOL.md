# Claude <-> OpenCode bridge (file mailbox)

Folders (in `bridge/`, not committed):
- `to-opencode/NNN-name.md`  a task for OpenCode (written by Claude, or by the user)
- `to-claude/NNN-name.md`    a reply or a new task for Claude (written by the worker or by OpenCode)
- `work/` the task being run, `done/` finished tasks

Message format: first lines `From: claude|opencode|user`, `To:`, `Re: <id or none>`, `Kind: task|reply`, then free text.

Rules for OpenCode (also prepended to every task by `oc-worker.sh`):
- research and drafts only: do NOT run git, fastboot, adb, flash, or touch the phone; do NOT download;
- NEVER touch anything tied to iOS activation (patched mobileactivationd, data_ark, purplebuddy/migration/springboard prefs, ADT Hactivation flags, `sweetfilepatch=` args);
- write results as files under `logs/research/` or `bridge/drafts/`, never edit `xnu/patches/*` or NOTES.md directly (Claude applies, tests and commits);
- to send Claude a task or a question, create `bridge/to-claude/<next-number>-<name>.md` in the format above.
Only the baton holder (Claude) commits, pushes, dispatches CI and touches the phone.
