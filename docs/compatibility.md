# arxsh compatibility

What arxsh v0.2.1 does and does not speak. arxsh does not claim bash
compatibility; this document is the contract. It is tested against the
regression suite (`make test`), and anything listed as supported that
breaks is a bug.

## Supported

### Commands and pipelines
- Simple commands with PATH lookup via `execvp`
- Pipelines of any reasonable length (`cmd1 | cmd2 | cmd3`)
- Redirection: `<`, `>`, `>>`, combinable with pipes
- Background execution with `&`
- Comments: `#` starts a comment outside quotes

### Quoting and expansion
- Single quotes (fully literal), double quotes (grouping, `$` still expands)
- Backslash escapes the next character
- `$VAR`, `${VAR}`, `$?` expansion
- `~` expansion at the start of a word
- Globbing for words containing `*` or `?` (via `glob(3)`); no match means
  the word stays literal
- Aliases (textual, first word, expanded once per name)

### Job control
- Process groups per pipeline, terminal handoff with `tcsetpgrp`
- `jobs`, `fg`, `bg` builtins; Ctrl-C interrupts, Ctrl-Z suspends
- Exit statuses: 127 for command not found, 126 for non-executable,
  128+signal for signal deaths, 2 for shell syntax errors

### Interactive
- Raw-mode line editing: history on arrow keys, Ctrl-A/E/U/K/W/L,
  Ctrl-C discards the line (status 130), Ctrl-D on an empty line exits
- Ghost suggestions from history (Right arrow / Ctrl-E / Ctrl-F accepts)
- Live tinting of the command word, right prompt (clock, jobs, exit code)
- History persisted to `~/.arxsh_history`; `~/.arxshrc` sourced at startup

### Builtins
`cd` (with `-` and `$HOME` fallback), `exit`, `help`, `export`, `unset`,
`alias`, `unalias`, `jobs`, `fg`, `bg`, `history`

## Not supported (yet)

| Feature | Planned |
|---|---|
| `&&`, `||`, `;` command lists | v0.3 |
| Command substitution `$(...)`, backticks | v0.3 |
| Here-documents, here-strings | v0.3 |
| Arithmetic expansion `$((...))` | Later |
| Shell functions, `if`/`for`/`while` | Later |
| Tab completion | v0.8 |
| Ctrl-R history search | v0.8 |
| `[...]` glob classes, `**` recursive glob | Later |
| pipefail | Later |
| Multi-line input / continuation prompts | Later |

## Deliberate differences from bash

- An unterminated quote closes at end of line instead of opening a
  continuation prompt.
- Typing a folder name on its own cds into it (auto-cd), when it does not
  shadow a runnable command.
- A mistyped command prints a did-you-mean suggestion (Damerau distance).
- The exit status of a pipeline is the last stage's status, as in bash
  without pipefail.
- Job completion notices appear at the next prompt, not asynchronously.
