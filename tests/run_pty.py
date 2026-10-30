#!/usr/bin/env python3
"""arxsh PTY suite: the interactive surface in a real terminal.

Covers the baseline contract (SPEC + strategy Phase 0): banner, command
execution, history recall, editor Ctrl-C, ghost suggestions, syntax tint,
right prompt, foreground Ctrl-C/Ctrl-Z job control, fg, duration notes.

Usage: ARXSH_BIN=/path/to/arxsh python3 tests/run_pty.py
"""
import os
import pty
import select
import signal
import sys
import time

BIN = os.environ.get("ARXSH_BIN", "./arxsh")
results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("ok   - " if cond else "FAIL - ") + name)


class Shell:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(BIN, [BIN])

    def drain(self, t=0.9):
        out = b""
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                out += chunk
        return out

    def send(self, data, wait=0.9):
        os.write(self.fd, data)
        return self.drain(wait)

    def close(self):
        try:
            os.write(self.fd, b"exit\r")
        except OSError:
            pass
        self.drain(0.6)
        wpid, _ = os.waitpid(self.pid, os.WNOHANG)
        if wpid == 0:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)


def main():
    sh = Shell()
    banner = sh.drain(1.2)
    check("banner mentions ghost suggestions", b"ghost suggestions" in banner)

    out = sh.send(b"echo pty-works\r", 1.2)
    check("runs a command", b"pty-works" in out)

    out = sh.send(b"\x1b[A", 0.8)  # Up: recall
    check("up arrow recalls history", b"pty-works" in out)
    out = sh.send(b"\r", 1.2)
    check("recalled command reruns", b"pty-works" in out)

    sh.send(b"echo cancelled", 0.4)
    out = sh.send(b"\x03", 0.8)  # Ctrl-C in the editor
    check("editor ctrl-c discards line with ^C", b"^C" in out)

    out = sh.send(b"ech", 1.0)
    check("ghost suggestion renders", b"o pty-works" in out)
    check("unknown prefix tints red", b"\x1b[1;31mech" in out)
    sh.send(b"\x1b[C", 0.6)  # Right arrow accepts the ghost
    out = sh.send(b"\r", 1.2)
    check("ghost accept reruns suggestion", b"pty-works" in out)

    out = sh.send(b"nosuchcmd42\r", 1.2)
    check("right prompt shows exit 127", b"exit 127" in out)

    out = sh.send(b"gti", 0.8)
    check("invalid command tints red", b"\x1b[1;31mgti" in out)
    sh.send(b"\x15", 0.4)  # Ctrl-U

    out = sh.send(b"sleep 2\r", 3.4)
    check("slow command prints duration", b"took 2." in out)

    # job control: Ctrl-C on a foreground job
    sh.send(b"sleep 30\r", 0.8)
    sh.send(b"\x03", 1.2)
    out = sh.send(b"echo alive-$?\r", 1.0)
    check("fg ctrl-c returns prompt, status 130", b"alive-130" in out)

    # job control: Ctrl-Z stops, jobs reports Stopped, fg resumes
    sh.send(b"sleep 30\r", 0.8)
    out = sh.send(b"\x1a", 1.5)
    check("ctrl-z prints Stopped notice", b"Stopped" in out)
    out = sh.send(b"jobs\r", 1.2)
    check("jobs shows job Stopped (not Running)", b"Stopped" in out and b"sleep 30" in out)
    sh.send(b"fg\r", 1.0)
    sh.send(b"\x03", 1.2)
    out = sh.send(b"echo back-$?\r", 1.0)
    check("fg then ctrl-c returns status 130", b"back-130" in out)

    sh.close()
    failed = [n for n, c in results if not c]
    print(f"pty: {len(results) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
