# Changelog

## v0.2.1 — 2026-10-30

Baseline stabilization (strategy Phase 0). No behaviour changes except the
process-lifecycle fix.

- Regression suite: `tests/run_integration.sh` (24 scripted cases) and
  `tests/run_pty.py` (15 interactive cases), wired as `make test`;
  `make test-asan` runs the integration suite under ASan/UBSan.
- Centralized child-status reaper: `waitpid` now happens in exactly one
  place and statuses are recorded by PID. Fixes the race where a foreground
  wait could consume a background job's exit status and lose it.
- Partial pipeline failure now kills and reaps whatever already started.
- Strict build (`-Wall -Wextra -Wpedantic -Werror`) clean; sanitizer run clean.
- CI: GitHub Actions, gcc + clang matrix, strict build, full test suite,
  plus a sanitizer job.
- `docs/STRATEGY.md` adopted: observability-first direction, milestones
  v0.3 to v1.0.

## v0.2.0 — 2026-10-30

The full shell plus the signature interactive layer.

- Core: REPL, hand-written tokenizer/parser, pipes, redirection, real job
  control (process groups, tcsetpgrp, jobs/fg/bg), quoting, `$VAR`/`$?`/`~`
  expansion, globbing, aliases, `~/.arxshrc`, persistent history, raw-mode
  line editor with arrow-key history.
- Signature: ghost history suggestions (Right-arrow accept), live syntax
  tinting (green = runnable, red = not), did-you-mean via Damerau distance,
  auto-cd on folder names, right prompt (clock, jobs, exit code), duration
  notes for commands over 2s, startup banner.
- Fixes from PTY testing: editor disables ISIG so Ctrl-C reaches it as a
  byte; job sweep no longer misreads an already-reported stop as Running.

## v0.1.0 — 2026-10-30

Public scaffold: README, five-phase SPEC, Makefile, MIT license, logo.
