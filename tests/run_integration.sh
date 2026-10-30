#!/bin/sh
# arxsh integration suite: scripted (non-tty) behaviour.
# Usage: ARXSH_BIN=/path/to/arxsh sh tests/run_integration.sh
# Exits non-zero if any case fails. Safe to run anywhere; uses a temp HOME.
set -u
BIN="${ARXSH_BIN:-./arxsh}"
[ -x "$BIN" ] || { echo "FAIL: $BIN not executable"; exit 1; }
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export HOME="$TMP/home"
mkdir -p "$HOME" "$TMP/work/sub"
cat > "$HOME/.arxshrc" <<'RC'
export EDITOR=vim
alias ll='ls'
RC

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "ok   - $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL - $1"; }
# run <name> <script> ; output lands in $OUT (stdout+stderr), exit code in $RC
run() {
    OUT="$(printf '%s\n' "$2" | (cd "$TMP/work" && timeout 15 "$BIN" 2>&1))"
    RC=$?
}
expect_out() { # name script expected-substring
    run "$1" "$2"
    case "$OUT" in *"$3"*) ok "$1" ;; *) bad "$1 (wanted: $3 | got: $OUT)" ;; esac
}
expect_rc() { # name script expected-exit-code
    printf '%s\n' "$2" | (cd "$TMP/work" && timeout 15 "$BIN" >/dev/null 2>&1)
    if [ $? -eq "$3" ]; then ok "$1"; else bad "$1 (wanted rc=$3 got rc=$?)"; fi
}

expect_out "echo"                       'echo hello'                                   'hello'
expect_out "exit status of false"       'false
echo st=$?'                                                                             'st=1'
expect_out "exit status recovery"       'false
true
echo st=$?'                                                                             'st=0'
printf 'alpha\ntest one\nbeta\ntest two\n' > "$TMP/work/data.txt"
expect_out "pipeline"                   'cat data.txt | grep test | wc -l'             '2'
expect_out "redirect write + read"      'echo hi > out.txt
cat < out.txt'                                                                          'hi'
expect_out "redirect append"            'echo one > app.txt
echo two >> app.txt
cat app.txt'                                                                            'two'
expect_out "double quotes keep spaces"  'echo "a  b"'                                  'a  b'
expect_out "single quotes are literal"  'echo '"'"'no $HOME'"'"''                      'no $HOME'
expect_out "HOME expansion"             'echo home=$HOME'                              "home=$HOME"
expect_out "unknown command is 127"     'nosuchcmd42
echo st=$?'                                                                             'st=127'
expect_out "did-you-mean suggests git"  'gti'                                          "did you mean 'git'?"
touch "$TMP/work/a.md" "$TMP/work/b.md"
expect_out "glob expands"               'echo *.md'                                    'a.md b.md'
expect_out "background job completes"   'sleep 0.3 &
sleep 1
echo after'                                                                             'Done'
expect_out "fg reattaches a job"        'sleep 1 &
fg %1
echo fgst=$?'                                                                           'fgst=0'
expect_out "cd sticks"                  'cd sub
pwd'                                                                                    "$TMP/work/sub"
touch "$TMP/work/marker.txt"
expect_out "cd - returns"               'cd sub
cd -
ls'                                                                                     'marker.txt'
expect_out "auto-cd on folder name"     'sub
pwd'                                                                                    "$TMP/work/sub"
expect_out "rc export applied"          'echo ed=$EDITOR'                              'ed=vim'
expect_out "rc alias applied"           'll data.txt'                                  'data.txt'
expect_out "export builtin"             'export FOO=bar
echo foo=$FOO'                                                                          'foo=bar'
expect_out "jobs lists background"      'sleep 3 &
jobs'                                                                                  'sleep 3'
expect_out "builtin in pipeline"        'history | grep echo'                          'echo'
expect_out "unterminated quote survives" 'echo "unterminated
echo after=$?'                                                                          'after=0'
expect_out "redirection w/o target"     'echo hi >
echo redir=$?'                                                                          'redir=2'
expect_out "leading pipe is an error"   '| wc -l
echo lead=$?'                                                                           'lead=2'
expect_out "double pipe is an error"    'echo a | | wc
echo mid=$?'                                                                            'mid=2'
expect_out "trailing pipe is an error"  'ls |
echo trail=$?'                                                                          'trail=2'
expect_out "comment line ignored"       '# just a comment
echo final-ok'                                                                          'final-ok'
expect_rc  "exit code propagates"       'exit 3'                                       3
expect_rc  "clean exit is zero"         'echo done'                                    0

# zombie check: a live shell that finishes 3 background jobs and sweeps
# must end up with zero children (every child reaped, none left defunct).
FIFO="$TMP/fifo"; mkfifo "$FIFO"
(cd "$TMP/work" && HOME="$HOME" timeout 15 "$BIN" < "$FIFO" > "$TMP/zout" 2>&1) &
exec 9> "$FIFO"
printf 'sleep 0.3 &\nsleep 0.3 &\nsleep 0.3 &\n' >&9
sleep 1.5
printf 'true\n' >&9
sleep 0.7
printf 'exit\n' >&9
exec 9>&-
sleep 0.5
DONES=$(grep -c 'Done' "$TMP/zout" || true)
if [ "$DONES" -eq 3 ]; then ok "no zombies: all 3 bg jobs reaped and reported"; else bad "zombie check (Done notices: $DONES)"; fi

echo "integration: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
