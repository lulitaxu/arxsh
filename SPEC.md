# arxsh build spec

House rule: Arz writes the code. Muse specs, reviews, and refuses to type the implementation.
Each phase ends with its acceptance tests passing and a commit.

## Phase 1 — Core
Concepts: process lifecycle (fork, execvp, waitpid), tokenizing, PATH lookup via execvp, errno.
Build:
- REPL: print prompt, read a line with getline(), EOF (Ctrl-D) exits cleanly
- Split the line into argv on whitespace (no quoting yet)
- fork(); child execvp(); parent waitpid() and records the exit status
- Builtins run in-process: cd (with $HOME fallback and `cd -`), exit, help
- execvp failure prints `arxsh: command not found: <cmd>` (ENOENT) or the strerror reason
Acceptance:
- `ls -la`, `pwd`, `whoami` run and match bash output
- `cd /tmp` then `pwd` prints /tmp; bare `cd` goes home
- Unknown command errors once, shell survives, exit status reflects failure
- Ctrl-D at an empty prompt exits with status 0

## Phase 2 — Plumbing
Concepts: pipe(), dup2(), fd hygiene (close every unused end, in parent too).
- `cmd1 | cmd2 | cmd3` chains
- `>`, `>>`, `<` redirection, combinable with pipes
Acceptance: `ls | grep .c | wc -l` correct; `echo hi > f` then `cat < f` prints hi; no fd leaks over 100 piped commands (check /proc/self/fd count in a test run)

## Phase 3 — Job control
Concepts: process groups (setpgid), terminal control (tcsetpgrp), signals SIGINT/SIGTSTP/SIGCHLD reaping.
- `&` background jobs, jobs/fg/bg builtins, job table with states
- Ctrl-C kills the foreground job, never the shell; Ctrl-Z suspends it
Acceptance: `sleep 30 &` returns instantly, `jobs` lists it, `fg` reattaches, Ctrl-C ends it and the prompt returns

## Phase 4 — Feel
- Single/double quotes and backslash escapes in the tokenizer
- $VAR and $? expansion, ~ expansion
- Globbing via glob()
- Raw-mode line editing (termios): arrows through history, Ctrl-A/E/U/K, history kept in memory and ~/.arxsh_history
Acceptance: `echo "a  b"` prints `a  b`; `echo $?` after a failed command prints nonzero; Up arrow recalls

## Phase 5 — Home
- ~/.arxshrc sourced at startup, aliases, PS1-style prompt config
