# arxsh — Production Development Strategy

Strategic direction, adopted 2026-10-30. This document governs development.
SPEC.md holds the per-phase build detail; this file holds the why and the order.

## Identity

**arxsh is an observability-first Unix shell written in C.** Not an AI wrapper
around bash, not a zsh clone with a different prompt, not a terminal emulator.
It understands its own commands, processes, pipelines, execution history, and
system interactions, and exposes that understanding natively.

When arxsh runs a pipeline, it already knows which processes it created, which
pipes it connected, and which exit codes it received. Instead of reconstructing
that afterwards, arxsh records it as it happens. That is the Execution
Observatory, and it is the reason this project exists.

## Product principles

1. No unnecessary AI dependency.
2. No sacrificing shell correctness for visual features.
3. No silently changing user commands.
4. No unnecessary resource consumption.
5. All intelligence features must be explainable.
6. Existing Unix tools must continue working normally.
7. Features are optional when they change execution behaviour.

## Components

| Component | Purpose |
|---|---|
| Shell Core | Execute commands reliably |
| Execution Engine | Manage processes, pipelines, jobs |
| Intelligence Engine | Explain and inspect execution |
| Interactive Engine | Provide a distinctive terminal experience |
| Security Engine | Detect potentially dangerous commands |
| Session Engine | Record and replay command sessions |
| Extension Engine | Allow external functionality |

The intelligence commands live in a colon namespace (`:trace`, `:explain`,
`:session`, `:which`, `:plugins`) so they never collide with real programs.

## The flagship

**Execution Replay with Evidence.** After a failure:

```
:inspect last
```

reconstructs what ran, which executable actually ran, which child processes
were created, which stage failed, each exit status, and how long each process
took, all from data the shell recorded at execution time. Then:

```
:explain last
```

explains the failure from that recorded evidence. The three defining features
develop as one connected system:

1. **Execution Observatory** — understand exactly what happened.
2. **Command Intelligence** — understand what a command will do.
3. **Session Time Travel** — understand what happened previously.

## Milestones

| Release | Scope |
|---|---|
| v0.2.1 | Baseline stabilization: regression suite, process-lifecycle fix, CI |
| v0.3 | Core refactoring: modules, explicit AST, `&&`/`||`/`;`, centralized process management |
| v0.4 | Execution Observatory: event system, traces, pipeline reconstruction, JSON export |
| v0.5 | Command Intelligence: `:explain`, rule-based risk engine, execution preview, `:which` |
| v0.6 | Session Time Travel: session store, timeline, explicit contextual replay |
| v0.7 | Plugin architecture: external executable plugins, versioned JSON protocol |
| v0.8 | UX and compatibility polish: completion, themes, git awareness, documented compatibility |
| v1.0 | Production release: CI green, sanitizer-clean, packaging, docs, reproducible builds |

Rough estimate: 12–16 weeks of focused part-time work. Sequencing note
(2026-10-30): the v0.2.1 sprint runs now; heavier phases queue behind the
Meta hackathon submission (Nov 4) and Haven Dhaka (Nov 13–14). If anything
slips, plugins (v0.7) slip first, never the Observatory.

## Phase 0 rules (baseline)

- Record current behaviour before changing it; the regression suite is the
  contract.
- Build strict: `-Wall -Wextra -Wpedantic -Werror`; run ASan/UBSan builds.
- Known bug to fix centrally: ad-hoc `waitpid(-1, ...)` calls can consume
  child-status events belonging to other jobs. Child-status collection is
  centralized in one reaper that records statuses by PID; waiters and the
  job sweep read from the reaper, never call `waitpid` directly.
- Release gate for process work: no process leaks, no zombies, no broken
  foreground/background transitions in the suite.

## Observatory design notes

- Event model: COMMAND_START, PROCESS_START, PIPE_CREATED, PROCESS_EXIT,
  COMMAND_END; every event carries execution ID, PID, PGID, timestamp,
  command index, exit status.
- The event system is observational only; it never modifies execution.
- Pipeline processes are **siblings**, not a parent-child chain, in every
  visualization.
- Tracing spawns no extra processes and can be disabled entirely.

## Command Intelligence design notes

- Rule-based first, on parsed structure, not string matching.
- Risk categories: destructive, privilege escalation, permission exposure,
  disk operations, remote execution (download piped to shell), system
  modification.
- Unresolved variables or aliases produce an *uncertain* assessment, never a
  false safety guarantee.
- Previews never silently rewrite a command.

## Session design notes

- Store command text, execution ID, working directory, timestamp, exit
  status, duration, session ID.
- Replay is always explicit; nothing destructive ever auto-reruns.
- Local storage by default, configurable retention, sensitive values
  redacted, no environment dumps, no raw input recording unless enabled.

## Plugin design notes

- First release: external executable plugins with a documented JSON
  input/output protocol (isolation at the process boundary).
- No arbitrary native shared-library loading as a trusted default.
- Versioned API, metadata, enable/disable configuration.

## Compatibility strategy

arxsh does not claim bash compatibility. docs/compatibility.md will define
supported syntax, builtins, expansion, job control, signal handling,
environment semantics, and known limitations, tested with controlled cases.
Predictable beats complete.

## Performance targets (measure, never estimate)

| Metric | Initial target |
|---|---|
| Idle memory | Under 10 MB |
| Cold startup | Under 50 ms |
| Simple command overhead | Under 5 ms |
| Observatory disabled | Negligible overhead |
| Core dependencies | Minimal |

## What NOT to build yet

- AI chatbot inside the terminal
- Built-in package manager
- Full terminal emulator
- Bash compatibility layer
- Cloud-synchronized history
- GUI configuration app
- Complex animation engine
- Native plugins without a security model

The biggest risk is scope explosion. Finish the execution architecture
before building more features on top of it.

## Positioning

Process tracing, suggestions, and safety tools exist in the wider Unix
ecosystem. arxsh's difference is their integration into one shell-native
execution model, not inventing each capability.

> ARXSH is an observability-first Unix shell written in C. It combines
> traditional shell execution with native process tracing, command
> intelligence, session reconstruction and an extensible architecture.
