# arxsh

<img src="brand/arxsh-logo-1024.png" width="120" alt="arxsh logo">

A Unix shell written from scratch in C. No frameworks, no borrowed parser, no readline. Just fork, exec, and an unhealthy curiosity about how the thing I type into all day actually works.

Built by [arxu](https://github.com/lulitaxu) on Arch Linux, in public, one phase at a time.

## Why

Every tool I use lives inside a shell, and every serious security exploit eventually negotiates with one: process control, file descriptors, PATH, signals, job control. Using bash daily without knowing how it forks a process felt like a gap. arxsh is how I close it, by rebuilding the tool instead of reading about it.

## Status

In active development. Phase 1 in progress.

## Roadmap

- [ ] **Phase 1 — Core.** Prompt loop, tokenizing input, `fork` + `execvp`, builtins (`cd`, `exit`, `help`), honest error messages
- [ ] **Phase 2 — Plumbing.** Pipes (`|`) and redirection (`>`, `>>`, `<`)
- [ ] **Phase 3 — Job control.** Background jobs (`&`), `jobs` / `fg` / `bg`, SIGINT and SIGTSTP handled the way a real shell handles them
- [ ] **Phase 4 — Feel.** Quoting and escapes, `$VAR` expansion, globbing, history and raw-mode line editing (arrow keys, Ctrl-A/E/U)
- [ ] **Phase 5 — Home.** `~/.arxshrc` startup file, aliases, a prompt that earns its place

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
