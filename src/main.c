/* arxsh — a Unix shell written from scratch in C.
 *
 * Phase 1 skeleton. The loop to build:
 *   1. print the prompt (only when stdin is a tty)
 *   2. read a line with getline(); EOF exits cleanly
 *   3. tokenize into argv (whitespace split for now)
 *   4. builtins (cd, exit, help) run in THIS process
 *   5. everything else: fork(), execvp() in the child,
 *      waitpid() in the parent, keep the exit status
 *
 * Spec: see SPEC.md, Phase 1 acceptance tests.
 */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* TODO(phase-1): the loop described above. */
    return 0;
}
