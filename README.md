# arxsh

<img src="brand/arxsh-logo-1024.png" width="120" alt="arxsh logo">

A Unix shell written from scratch in C. No frameworks, no borrowed parser, no readline. Just fork, exec, and an unhealthy curiosity about how the thing I type into all day actually works.

Built by [arxu](https://github.com/lulitaxu) on Arch Linux, in public, one phase at a time.

## Why

Every tool I use lives inside a shell, and every serious security exploit eventually negotiates with one: process control, file descriptors, PATH, signals, job control. Using bash daily without knowing how it forks a process felt like a gap. arxsh is how I close it, by rebuilding the tool instead of reading about it.

## Status

Version 0.2.1. All five build phases implemented, plus a signature interactive layer no stock shell ships with. arxsh is a working daily-driver shell, still growing.

Where this is going: [docs/STRATEGY.md](docs/STRATEGY.md) — arxsh is becoming an observability-first shell (native execution traces, command intelligence, session replay; the Execution Observatory lands in v0.4). Run the regression suite with `make test`.

## Features

- Hand-written tokenizer and parser: no readline, no borrowed shell code
- Pipes and redirection: `|`, `>`, `>>`, `<`, combinable in one line
- Real job control: process groups, terminal handoff (`tcsetpgrp`), `&` background jobs, `jobs` / `fg` / `bg`, Ctrl-C and Ctrl-Z behave the way they should
- Quoting and escapes: single quotes, double quotes, backslash
- Expansion: `$VAR`, `${VAR}`, `$?`, `~`, and globbing (`*`, `?`)
- Line editing in raw mode: arrow-key history, Ctrl-A/E/U/K/W/L, history persisted to `~/.arxsh_history`
- Builtins: `cd`, `exit`, `help`, `export`, `unset`, `alias`, `unalias`, `jobs`, `fg`, `bg`, `history`
- `~/.arxshrc` startup file, aliases, and a prompt that shows the last exit status when a command fails

## What makes it different

Stock shells wait for you to press Enter before telling you anything. arxsh talks back while you type:

- **Ghost suggestions.** Start typing and arxsh completes the line from your history in dim text. Right arrow takes the whole suggestion, and you keep typing to ignore it.
- **Live tinting.** The command name is green when arxsh can actually run it and red when it cannot, checked against builtins, aliases, your PATH, and folders as you type. Quoted strings glow amber, operators stand out bold.
- **Did-you-mean.** Mistype a command and arxsh guesses the fix: `gti` gets "did you mean 'git'?", scored with Damerau edit distance (a letter swap counts as one slip, the way fingers actually make it) over builtins, aliases, and every executable on your PATH.
- **Auto-cd.** Type a folder name on its own and you are inside it. No `cd`, no ceremony.
- **A right prompt with the truth in it.** Clock, how many jobs you have open, and the exact exit code of the last command when it failed.
- **Receipts for slow commands.** Anything that takes two seconds or more reports how long it took, right when it finishes.

## Roadmap

- [x] **Phase 1 — Core.** Prompt loop, tokenizing input, `fork` + `execvp`, builtins (`cd`, `exit`, `help`), honest error messages
- [x] **Phase 2 — Plumbing.** Pipes (`|`) and redirection (`>`, `>>`, `<`)
- [x] **Phase 3 — Job control.** Background jobs (`&`), `jobs` / `fg` / `bg`, SIGINT and SIGTSTP handled the way a real shell handles them
- [x] **Phase 4 — Feel.** Quoting and escapes, `$VAR` expansion, globbing, history and raw-mode line editing (arrow keys, Ctrl-A/E/U)
- [x] **Phase 5 — Home.** `~/.arxshrc` startup file, aliases, a prompt that earns its place

## Known limitations (v0.2.1)

arxsh is a working shell, not a bash replacement yet. The honest list:

- No command lists: `&&`, `||`, and `;` are not parsed yet (v0.3, top priority)
- No command substitution (`$(...)`, backticks), here-docs, arithmetic expansion, or shell functions
- No control structures (`if`, `for`, `while`): arxsh runs commands, it is not a scripting language yet
- No tab completion: ghost suggestions and history fill that gap for now
- An unterminated quote closes at end of line instead of continuing to the next line
- Globbing triggers on `*` and `?` only (no `[...]` classes), and a pattern with no match stays literal
- A pipeline's exit status is its last stage's (no pipefail)
- Background job Done/Stopped notices print at the next prompt, not asynchronously mid-typing
- No Ctrl-R history search; history is saved on exit, so a hard kill loses that session's entries
- Custom `PS1` supports `\w`, `\u`, `\$` escapes only

Full detail: [docs/compatibility.md](docs/compatibility.md). Direction: [docs/STRATEGY.md](docs/STRATEGY.md).

## Build and run

```sh
make
./arxsh
```

Requires a C compiler and a POSIX system. Developed on Arch Linux.

## Design notes

- One process per command, forked from the shell, waited on by the shell. Builtins are the only commands that run inside the shell process itself, because `cd` in a child would change nothing.
- The parser is hand-written: a shell that cannot tokenize its own input is a launcher, not a shell.
- Errors are reported like a tool, not a homework exercise: `arxsh: command not found: xyz`, with the exit status kept in `$?` once expansion lands.

## License

MIT
