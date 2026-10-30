#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <glob.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define ARXSH_VERSION "0.2.1"

extern char **environ;

#define MAX_CMDS 16
#define MAX_JOBS 32
#define MAX_ALIASES 64

static int last_status = 0;
static pid_t shell_pgid;

/* ---------- small dynamic string ---------- */
typedef struct { char *buf; size_t len, cap; } sb_t;
static void sb_init(sb_t *s) { s->cap = 64; s->len = 0; s->buf = malloc(s->cap); s->buf[0] = 0; }
static void sb_putc(sb_t *s, char c) {
    if (s->len + 2 > s->cap) { s->cap *= 2; s->buf = realloc(s->buf, s->cap); }
    s->buf[s->len++] = c; s->buf[s->len] = 0;
}
static void sb_puts(sb_t *s, const char *t) { while (*t) sb_putc(s, *t++); }
static void sb_free(sb_t *s) { free(s->buf); s->buf = NULL; s->len = s->cap = 0; }

static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) { perror("strdup"); exit(1); } return p; }

/* ---------- jobs ---------- */
typedef struct {
    int used;
    int id;
    pid_t pgid;
    pid_t pids[MAX_CMDS];
    int npids;
    int stopped;
    int notified;
    char *cmdline;
} job_t;
static job_t jobs[MAX_JOBS];
static int next_job_id = 1;

static job_t *job_add(pid_t pgid, pid_t *pids, int npids, const char *cmdline, int stopped) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            jobs[i].used = 1;
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].npids = npids;
            memcpy(jobs[i].pids, pids, sizeof(pid_t) * npids);
            jobs[i].stopped = stopped;
            jobs[i].notified = 0;
            jobs[i].cmdline = xstrdup(cmdline);
            return &jobs[i];
        }
    }
    return NULL;
}
static job_t *job_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].id == id) return &jobs[i];
    return NULL;
}
static job_t *job_last(void) {
    job_t *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && (!best || jobs[i].id > best->id)) best = &jobs[i];
    return best;
}
static void job_remove(job_t *j) { free(j->cmdline); j->cmdline = NULL; j->used = 0; }

/* ---------- child-status reaper ----------
   The ONLY place waitpid is called. Statuses are recorded by PID and read
   back with reap_peek(); records live until reap_drop(). This replaces the
   old pattern where every waiter called waitpid(-1) itself and could
   consume a status event belonging to a different job, losing it. */
#define MAX_REAPED 1024
typedef struct { pid_t pid; int status; int has_status; int used; } reap_t;
static reap_t reap_table[MAX_REAPED];
static int reap_no_children = 0;

static reap_t *reap_slot(pid_t pid, int create) {
    reap_t *free_slot = NULL;
    for (int i = 0; i < MAX_REAPED; i++) {
        if (reap_table[i].used && reap_table[i].pid == pid) return &reap_table[i];
        if (!reap_table[i].used && !free_slot) free_slot = &reap_table[i];
    }
    if (create && free_slot) { free_slot->used = 1; free_slot->pid = pid; free_slot->has_status = 0; free_slot->status = 0; return free_slot; }
    return NULL;
}
static void reap_watch(pid_t pid) { (void)reap_slot(pid, 1); }
static void reap_drop(pid_t pid) { reap_t *s = reap_slot(pid, 0); if (s) s->used = 0; }
static int reap_peek(pid_t pid, int *out_status) {
    reap_t *s = reap_slot(pid, 0);
    if (s && s->has_status) { *out_status = s->status; return 1; }
    return 0;
}
static void reap_children(void) {
    int st; pid_t r;
    reap_no_children = 0;
    while ((r = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        reap_t *s = reap_slot(r, 1);
        if (s) { s->status = st; s->has_status = 1; }
    }
    if (r < 0 && errno == ECHILD) reap_no_children = 1;
}

/* sweep background jobs before each prompt; report finished/stopped ones */
static void jobs_sweep(void) {
    reap_children();
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        job_t *j = &jobs[i];
        int alive = 0, newly_stopped = 0;
        for (int k = 0; k < j->npids; k++) {
            int st;
            if (reap_peek(j->pids[k], &st)) {
                if (WIFSTOPPED(st)) { newly_stopped = 1; alive++; }
                else if (WIFCONTINUED(st)) { j->stopped = 0; alive++; }
                /* exited/signaled: terminal record, counts as done */
            } else alive++;
        }
        if (!alive) {
            printf("[%d] Done\t\t%s\n", j->id, j->cmdline);
            for (int k = 0; k < j->npids; k++) reap_drop(j->pids[k]);
            job_remove(j);
        } else if (newly_stopped && !j->stopped) {
            j->stopped = 1;
            printf("[%d] Stopped\t\t%s\n", j->id, j->cmdline);
        }
    }
    fflush(stdout);
}

/* ---------- history ---------- */
#define MAX_HISTORY 1000
static char *history_list[MAX_HISTORY];
static int history_count = 0;
static void history_add(const char *line) {
    if (!line || !*line) return;
    if (history_count > 0 && strcmp(history_list[history_count - 1], line) == 0) return;
    if (history_count >= MAX_HISTORY) {
        free(history_list[0]);
        memmove(history_list, history_list + 1, sizeof(char *) * (MAX_HISTORY - 1));
        history_count = MAX_HISTORY - 1;
    }
    history_list[history_count++] = xstrdup(line);
}
static char *history_path(void) {
    const char *home = getenv("HOME"); if (!home) home = ".";
    char *p = malloc(strlen(home) + 32);
    sprintf(p, "%s/.arxsh_history", home);
    return p;
}
static void history_load(void) {
    char *p = history_path();
    FILE *f = fopen(p, "r"); free(p);
    if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > 0) history_add(line);
    }
    free(line); fclose(f);
}
static void history_save(void) {
    char *p = history_path();
    FILE *f = fopen(p, "w"); free(p);
    if (!f) return;
    for (int i = 0; i < history_count; i++) fprintf(f, "%s\n", history_list[i]);
    fclose(f);
}

/* ---------- aliases ---------- */
typedef struct { char *name, *value; } alias_t;
static alias_t aliases[MAX_ALIASES];
static int alias_count = 0;
static const char *alias_get(const char *name) {
    for (int i = 0; i < alias_count; i++) if (strcmp(aliases[i].name, name) == 0) return aliases[i].value;
    return NULL;
}
static void alias_set(const char *name, const char *value) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) { free(aliases[i].value); aliases[i].value = xstrdup(value); return; }
    }
    if (alias_count < MAX_ALIASES) {
        aliases[alias_count].name = xstrdup(name);
        aliases[alias_count].value = xstrdup(value);
        alias_count++;
    }
}
static int alias_unset(const char *name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].name); free(aliases[i].value);
            aliases[i] = aliases[--alias_count];
            return 0;
        }
    }
    return 1;
}

/* ---------- interactive helpers ---------- */
static const char *builtin_names[] = { "cd", "exit", "help", "export", "unset", "alias", "unalias", "jobs", "fg", "bg", "history", NULL };
static int is_builtin(const char *name) {
    for (int i = 0; builtin_names[i]; i++) if (strcmp(name, builtin_names[i]) == 0) return 1;
    return 0;
}

static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return (int)ws.ws_col;
    return 80;
}

/* printable width of a string that may contain ANSI color sequences */
static int visible_len(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\033' && p[1] == '[') {
            p += 2;
            while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
            if (!*p) break;
            continue;
        }
        n++;
    }
    return n;
}

static int path_lookup(const char *name) {
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    char full[4352];
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen == 0) snprintf(full, sizeof(full), "./%s", name);
        else {
            if (dlen + strlen(name) + 2 > sizeof(full)) { if (!colon) break; p = colon + 1; continue; }
            memcpy(full, p, dlen); full[dlen] = '/'; strcpy(full + dlen + 1, name);
        }
        if (access(full, X_OK) == 0) return 1;
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

/* is this word runnable (builtin, alias, PATH hit, or a folder for auto-cd)? */
static int command_known(const char *w, size_t len) {
    static char cache[256]; static size_t cache_len = (size_t)-1; static int cache_val = 0;
    if (len == cache_len && len < sizeof(cache) && memcmp(cache, w, len) == 0) return cache_val;
    if (len >= 256) return 0;
    char name[256]; memcpy(name, w, len); name[len] = 0;
    int v = 0;
    struct stat st;
    if (is_builtin(name) || alias_get(name)) v = 1;
    else if (strchr(name, '/')) {
        if (access(name, X_OK) == 0 || (stat(name, &st) == 0 && S_ISDIR(st.st_mode))) v = 1;
    }
    else if (path_lookup(name)) v = 1;
    else if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) v = 1;
    memcpy(cache, w, len); cache_len = len; cache_val = v;
    return v;
}

/* newest history entry that starts with `line` and is longer than it */
static const char *find_suggestion(const char *line) {
    size_t len = strlen(line);
    if (len == 0) return NULL;
    for (int i = history_count - 1; i >= 0; i--)
        if (strncmp(history_list[i], line, len) == 0 && history_list[i][len] != 0) return history_list[i];
    return NULL;
}

/* Damerau distance (Levenshtein + adjacent transposition = 1, so the classic
   finger-slip "gti" lands on "git", not on some unrelated two-edit name) */
static int levenshtein(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 64 || lb > 64) return 99;
    int prev2[65], prev[65], cur[65];
    for (size_t j = 0; j <= lb; j++) { prev[j] = (int)j; prev2[j] = (int)j; }
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1;
            if (prev[j - 1] + cost < m) m = prev[j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && prev2[j - 2] + 1 < m)
                m = prev2[j - 2] + 1;
            cur[j] = m;
        }
        memcpy(prev2, prev, sizeof(int) * (lb + 1));
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

/* closest builtin/alias/PATH name within edit distance 2, or NULL */
static char *did_you_mean(const char *name) {
    int best = 3;
    char bestname[256] = "";
    for (int i = 0; builtin_names[i]; i++) {
        int d = levenshtein(name, builtin_names[i]);
        if (d < best) { best = d; snprintf(bestname, sizeof(bestname), "%s", builtin_names[i]); }
    }
    for (int i = 0; i < alias_count; i++) {
        int d = levenshtein(name, aliases[i].name);
        if (d < best) { best = d; snprintf(bestname, sizeof(bestname), "%s", aliases[i].name); }
    }
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    char dirbuf[4096]; snprintf(dirbuf, sizeof(dirbuf), "%s", path);
    char *save = NULL;
    for (char *dir = strtok_r(dirbuf, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir); if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            long diff = (long)strlen(e->d_name) - (long)strlen(name);
            if (diff < -2 || diff > 2) continue;
            int dist = levenshtein(name, e->d_name);
            if (dist < best) {
                char full[4352]; snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
                if (access(full, X_OK) == 0) { best = dist; snprintf(bestname, sizeof(bestname), "%s", e->d_name); }
            }
        }
        closedir(d);
    }
    if (best <= 2 && bestname[0]) return xstrdup(bestname);
    return NULL;
}

/* render the input line with tints: command name green when arxsh can run it,
   red when it cannot, quoted strings amber */
static void render_tinted(sb_t *out, const char *buf) {
    size_t n = strlen(buf);
    size_t wstart = 0; while (wstart < n && buf[wstart] == ' ') wstart++;
    size_t wend = wstart;
    while (wend < n && buf[wend] != ' ' && buf[wend] != '|' && buf[wend] != '<' && buf[wend] != '>' && buf[wend] != '&') wend++;
    int known = (wend > wstart) && command_known(buf + wstart, wend - wstart);
    int in_sq = 0, in_dq = 0;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\'' && !in_dq) { in_sq = !in_sq; sb_puts(out, "\033[33m"); sb_putc(out, c); if (!in_sq) sb_puts(out, "\033[0m"); continue; }
        if (c == '"' && !in_sq) { in_dq = !in_dq; sb_puts(out, "\033[33m"); sb_putc(out, c); if (!in_dq) sb_puts(out, "\033[0m"); continue; }
        if (in_sq || in_dq) { sb_putc(out, c); continue; }
        if (i >= wstart && i < wend) {
            if (i == wstart) sb_puts(out, known ? "\033[1;32m" : "\033[1;31m");
            sb_putc(out, c);
            if (i + 1 == wend) sb_puts(out, "\033[0m");
            continue;
        }
        if (c == '|' || c == '<' || c == '>' || c == '&') { sb_puts(out, "\033[1m"); sb_putc(out, c); sb_puts(out, "\033[0m"); continue; }
        sb_putc(out, c);
    }
    if (in_sq || in_dq) sb_puts(out, "\033[0m");
}

/* right-hand prompt segment: clock, open jobs, failing exit status */
static void build_rprompt(sb_t *out) {
    int nj = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) nj++;
    time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
    char clockbuf[16]; strftime(clockbuf, sizeof(clockbuf), "%H:%M", &tmv);
    sb_puts(out, "\033[2m");
    sb_puts(out, clockbuf);
    if (nj > 0) { char jb[40]; snprintf(jb, sizeof(jb), " · %d job%s", nj, nj == 1 ? "" : "s"); sb_puts(out, jb); }
    sb_puts(out, "\033[0m");
    if (last_status != 0) { char eb[48]; snprintf(eb, sizeof(eb), " \033[1;31mexit %d\033[0m", last_status); sb_puts(out, eb); }
}

/* ---------- prompt ---------- */
static void build_prompt(sb_t *out) {
    const char *ps1 = getenv("PS1");
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    const char *home = getenv("HOME");
    char short_cwd[4096];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(short_cwd, sizeof(short_cwd), "~%s", cwd + strlen(home));
    else
        snprintf(short_cwd, sizeof(short_cwd), "%s", cwd);
    if (ps1) {
        for (const char *p = ps1; *p; p++) {
            if (p[0] == '\\' && p[1] == 'w') { sb_puts(out, short_cwd); p++; }
            else if (p[0] == '\\' && p[1] == 'u') { const char *u = getenv("USER"); sb_puts(out, u ? u : "user"); p++; }
            else if (p[0] == '\\' && p[1] == '$') { sb_putc(out, getuid() == 0 ? '#' : '$'); p++; }
            else sb_putc(out, *p);
        }
        return;
    }
    sb_puts(out, "\033[1;32marxsh\033[0m ");
    sb_puts(out, "\033[2m");
    sb_puts(out, short_cwd);
    sb_puts(out, "\033[0m ");
    if (last_status != 0) sb_puts(out, "\033[1;31m>\033[0m ");
    else sb_puts(out, "\033[1;32m>\033[0m ");
}

/* ---------- line editor (raw mode, tty only) ---------- */
static struct termios orig_termios;
static void term_restore(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

/* Returns a malloc'd line, or NULL on EOF (Ctrl-D on empty line). */
static char *read_line_tty(const char *prompt) {
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    sb_t line; sb_init(&line);
    size_t pos = 0;
    int hist = history_count; /* browsing index; == count means "current line" */
    char *saved = NULL;
    char sugg_full[4096]; sugg_full[0] = 0;   /* ghost suggestion, refreshed at redraw */

    fputs(prompt, stdout);
    {   /* paint the right segment on the fresh prompt too, not just on keystrokes */
        sb_t rp; sb_init(&rp); build_rprompt(&rp);
        int rp_len = visible_len(rp.buf);
        int width = term_width();
        int prompt_len = visible_len(prompt);
        if (width >= 50 && prompt_len + rp_len + 2 <= width)
            printf("\033[%dG%s\033[%dG", width - rp_len + 1, rp.buf, prompt_len + 1);
        sb_free(&rp);
    }
    fflush(stdout);
    for (;;) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) { term_restore(); sb_free(&line); free(saved); return NULL; }
        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout); fflush(stdout);
            term_restore();
            free(saved);
            return line.buf;
        }
        if (c == 4) { /* Ctrl-D */
            if (line.len == 0) { term_restore(); sb_free(&line); free(saved); return NULL; }
            continue;
        }
        if (c == 3) { /* Ctrl-C: discard the line */
            fputs("^C\r\n", stdout); fflush(stdout);
            term_restore();
            free(saved);
            last_status = 130;
            return xstrdup("");
        }
        if (c == 1) { pos = 0; }                    /* Ctrl-A */
        else if (c == 5 || c == 6) {                  /* Ctrl-E / Ctrl-F */
            if (pos == line.len && sugg_full[0]) {    /* accept the ghost suggestion */
                line.len = 0; line.buf[0] = 0; sb_puts(&line, sugg_full); pos = line.len;
            } else pos = line.len;
        }
        else if (c == 21) { line.len = 0; pos = 0; line.buf[0] = 0; }  /* Ctrl-U */
        else if (c == 11) { line.buf[pos] = 0; line.len = pos; }       /* Ctrl-K */
        else if (c == 23) {                            /* Ctrl-W */
            while (pos > 0 && line.buf[pos - 1] == ' ') { memmove(line.buf + pos - 1, line.buf + pos, line.len - pos + 1); pos--; line.len--; }
            while (pos > 0 && line.buf[pos - 1] != ' ') { memmove(line.buf + pos - 1, line.buf + pos, line.len - pos + 1); pos--; line.len--; }
        }
        else if (c == 12) { fputs("\033[2J\033[H", stdout); }  /* Ctrl-L */
        else if (c == 27) {                           /* escape sequences */
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
            if (seq[1] == 'A' || seq[1] == 'B') {     /* history up/down */
                if (seq[1] == 'A' && hist > 0) {
                    if (hist == history_count) { free(saved); saved = xstrdup(line.buf); }
                    hist--;
                } else if (seq[1] == 'B' && hist < history_count) {
                    hist++;
                } else goto redraw;
                const char *src = (hist == history_count) ? (saved ? saved : "") : history_list[hist];
                line.len = 0; line.buf[0] = 0;
                sb_puts(&line, src);
                pos = line.len;
            }
            else if (seq[1] == 'C') {                 /* Right arrow */
                if (pos < line.len) pos++;
                else if (sugg_full[0]) {              /* at end of line: accept the ghost */
                    line.len = 0; line.buf[0] = 0; sb_puts(&line, sugg_full); pos = line.len;
                }
            }
            else if (seq[1] == 'D') { if (pos > 0) pos--; }
            else if (seq[1] == 'H') { pos = 0; }
            else if (seq[1] == 'F') { pos = line.len; }
            else if (seq[1] == '3') {                 /* Delete key: ESC [ 3 ~ */
                char tilde; if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~' && pos < line.len) {
                    memmove(line.buf + pos, line.buf + pos + 1, line.len - pos);
                    line.len--;
                }
            }
        }
        else if (c == 127 || c == 8) {                /* Backspace */
            if (pos > 0) {
                memmove(line.buf + pos - 1, line.buf + pos, line.len - pos + 1);
                pos--; line.len--;
            }
        }
        else if (c >= 32 && c < 127) {                /* printable insert */
            if (line.len + 2 > line.cap) { line.cap *= 2; line.buf = realloc(line.buf, line.cap); }
            memmove(line.buf + pos + 1, line.buf + pos, line.len - pos + 1);
            line.buf[pos] = c; pos++; line.len++;
        }
    redraw:
        {
            /* ghost suggestion: dim tail of the newest matching history entry */
            const char *sugg = (pos == line.len) ? find_suggestion(line.buf) : NULL;
            sugg_full[0] = 0;
            if (sugg) snprintf(sugg_full, sizeof(sugg_full), "%s", sugg);
            sb_t rp; sb_init(&rp); build_rprompt(&rp);
            int rp_len = visible_len(rp.buf);
            sb_t body; sb_init(&body); render_tinted(&body, line.buf);
            int width = term_width();
            int prompt_len = visible_len(prompt);
            printf("\r\033[K");
            if (width >= 50 && prompt_len + (int)line.len + rp_len + 2 <= width)
                printf("\033[%dG%s\r", width - rp_len + 1, rp.buf);
            printf("%s%s", prompt, body.buf);
            size_t printed = line.len;
            if (sugg) { printf("\033[2m%s\033[0m", sugg + line.len); printed += strlen(sugg + line.len); }
            if (printed > pos) printf("\033[%zuD", printed - pos);
            fflush(stdout);
            sb_free(&rp); sb_free(&body);
        }
    }
}

/* ---------- tokenizer ---------- */
typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_BG, T_END } toktype_t;
typedef struct { toktype_t type; char *text; int quoted; } token_t;

/* expand $VAR, ${VAR}, $? at position *pp (points at '$'); appends to out */
static void expand_dollar(sb_t *out, const char **pp) {
    const char *p = *pp + 1;
    if (*p == '?') {
        char num[16]; snprintf(num, sizeof(num), "%d", last_status);
        sb_puts(out, num); *pp = p + 1; return;
    }
    if (*p == '{') {
        const char *start = ++p;
        while (*p && *p != '}') p++;
        char name[256]; size_t n = (size_t)(p - start); if (n > 255) n = 255;
        memcpy(name, start, n); name[n] = 0;
        const char *v = getenv(name);
        if (v) sb_puts(out, v);
        *pp = (*p == '}') ? p + 1 : p;
        return;
    }
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
        const char *start = p;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
        char name[256]; size_t n = (size_t)(p - start); if (n > 255) n = 255;
        memcpy(name, start, n); name[n] = 0;
        const char *v = getenv(name);
        if (v) sb_puts(out, v);
        *pp = p; return;
    }
    sb_putc(out, '$');
    *pp = p;
}

static void token_push(token_t **toks, int *n, int *cap, toktype_t type, char *text, int quoted) {
    if (*n + 1 >= *cap) { *cap *= 2; *toks = realloc(*toks, sizeof(token_t) * *cap); }
    (*toks)[*n].type = type; (*toks)[*n].text = text; (*toks)[*n].quoted = quoted;
    (*n)++;
}

/* Split a command line into tokens. Returns token array ending with T_END. */
static token_t *tokenize(const char *input) {
    int cap = 16, n = 0;
    token_t *toks = malloc(sizeof(token_t) * cap);
    const char *p = input;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '#') break;                 /* comment */
        if (*p == '|') { token_push(&toks, &n, &cap, T_PIPE, NULL, 0); p++; continue; }
        if (*p == '<') { token_push(&toks, &n, &cap, T_IN, NULL, 0); p++; continue; }
        if (*p == '&') { token_push(&toks, &n, &cap, T_BG, NULL, 0); p++; continue; }
        if (*p == '>') {
            if (p[1] == '>') { token_push(&toks, &n, &cap, T_APPEND, NULL, 0); p += 2; }
            else { token_push(&toks, &n, &cap, T_OUT, NULL, 0); p++; }
            continue;
        }
        /* a word */
        sb_t w; sb_init(&w);
        int quoted = 0, at_start = 1;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '<' && *p != '>' && *p != '&') {
            if (*p == '\'') {
                quoted = 1; p++;
                while (*p && *p != '\'') sb_putc(&w, *p++);
                if (*p == '\'') p++;
            } else if (*p == '"') {
                quoted = 1; p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { sb_putc(&w, p[1]); p += 2; }
                    else if (*p == '$') expand_dollar(&w, &p);
                    else sb_putc(&w, *p++);
                }
                if (*p == '"') p++;
            } else if (*p == '\\' && p[1]) {
                quoted = 1; sb_putc(&w, p[1]); p += 2;
            } else if (*p == '$') {
                expand_dollar(&w, &p);
            } else if (*p == '~' && at_start && (p[1] == '/' || p[1] == 0 || p[1] == ' ' || p[1] == '\t')) {
                const char *home = getenv("HOME");
                if (home) sb_puts(&w, home);
                p++;
            } else {
                sb_putc(&w, *p++);
            }
            at_start = 0;
        }
        token_push(&toks, &n, &cap, T_WORD, w.buf, quoted);
    }
    token_push(&toks, &n, &cap, T_END, NULL, 0);
    return toks;
}

static void tokens_free(token_t *toks) {
    for (int i = 0; toks[i].type != T_END; i++) free(toks[i].text);
    free(toks);
}

/* ---------- parser ---------- */
typedef struct {
    char **argv; int argc, argcap;
    char *in_path, *out_path;
    int append;
} cmd_t;

static void cmd_push_arg(cmd_t *c, char *arg) {
    if (c->argc + 2 > c->argcap) { c->argcap = c->argcap ? c->argcap * 2 : 8; c->argv = realloc(c->argv, sizeof(char *) * c->argcap); }
    c->argv[c->argc++] = arg;
    c->argv[c->argc] = NULL;
}

/* Parse tokens into a pipeline. Returns command count, or -1 on syntax error. */
static int parse_tokens(token_t *toks, cmd_t *cmds, int *background) {
    int nc = 0;
    *background = 0;
    memset(&cmds[0], 0, sizeof(cmd_t));
    for (int i = 0; toks[i].type != T_END; i++) {
        token_t *t = &toks[i];
        if (t->type == T_WORD) {
            /* glob unquoted words containing wildcards */
            if (!t->quoted && (strchr(t->text, '*') || strchr(t->text, '?'))) {
                glob_t g;
                if (glob(t->text, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
                    for (size_t k = 0; k < g.gl_pathc; k++) cmd_push_arg(&cmds[nc], xstrdup(g.gl_pathv[k]));
                    globfree(&g);
                    continue;
                }
                if (g.gl_pathc == 0) globfree(&g);
            }
            cmd_push_arg(&cmds[nc], t->text);
            t->text = NULL; /* ownership moved */
        } else if (t->type == T_PIPE) {
            if (cmds[nc].argc == 0 || nc + 1 >= MAX_CMDS) { fprintf(stderr, "arxsh: syntax error near '|'\n"); return -1; }
            nc++;
            memset(&cmds[nc], 0, sizeof(cmd_t));
        } else if (t->type == T_IN || t->type == T_OUT || t->type == T_APPEND) {
            if (toks[i + 1].type != T_WORD) { fprintf(stderr, "arxsh: syntax error: redirection without a target\n"); return -1; }
            i++;
            if (t->type == T_IN) cmds[nc].in_path = toks[i].text;
            else { cmds[nc].out_path = toks[i].text; cmds[nc].append = (t->type == T_APPEND); }
            toks[i].text = NULL;
        } else if (t->type == T_BG) {
            *background = 1;
        }
    }
    if (cmds[nc].argc == 0 && nc == 0) return 0;
    if (cmds[nc].argc == 0) { fprintf(stderr, "arxsh: syntax error: empty command\n"); return -1; }
    return nc + 1;
}

static void cmds_free(cmd_t *cmds, int nc) {
    for (int i = 0; i < nc; i++) {
        for (int k = 0; k < cmds[i].argc; k++) free(cmds[i].argv[k]);
        free(cmds[i].argv);
        free(cmds[i].in_path);
        free(cmds[i].out_path);
    }
}

/* ---------- builtins ---------- */
/* is_builtin lives with the interactive helpers (it shares builtin_names) */

/* wait for a set of pids (foreground) by polling the reaper.
   Returns 1 if any stopped. Records are NOT consumed here: the caller
   drops them once the command or job is fully finished, so a job that
   stops keeps the exit records of the stages that already finished. */
static int wait_for_pids(pid_t *pids, int npids, int *out_status) {
    int stopped = 0, status = 0;
    for (;;) {
        reap_children();
        int remaining = 0;
        for (int i = 0; i < npids; i++) {
            if (pids[i] <= 0) continue;
            int st;
            if (reap_peek(pids[i], &st)) {
                if (WIFSTOPPED(st)) stopped = 1;
                else if (WIFEXITED(st)) { if (i == npids - 1) status = WEXITSTATUS(st); }
                else if (WIFSIGNALED(st)) { if (i == npids - 1) status = 128 + WTERMSIG(st); }
                else remaining++;            /* continued: still running */
            } else remaining++;
        }
        if (stopped || remaining == 0) break;
        if (reap_no_children) break;        /* nothing left to wait for */
        { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }   /* 1 ms poll */
    }
    if (out_status) *out_status = status;
    return stopped;
}

static job_t *job_from_arg(const char *arg) {
    if (!arg) return job_last();
    if (arg[0] == '%') arg++;
    char *end; long id = strtol(arg, &end, 10);
    if (end == arg) return NULL;
    return job_by_id((int)id);
}

static int run_builtin(cmd_t *cmd) {
    const char *name = cmd->argv[0];
    char **a = cmd->argv;
    int ac = cmd->argc;
    if (strcmp(name, "exit") == 0) {
        int code = (ac > 1) ? atoi(a[1]) : last_status;
        history_save();
        exit(code);
    }
    if (strcmp(name, "cd") == 0) {
        const char *dir = (ac > 1) ? a[1] : getenv("HOME");
        if (ac > 1 && strcmp(a[1], "-") == 0) dir = getenv("OLDPWD");
        if (!dir) { fprintf(stderr, "arxsh: cd: no target\n"); return 1; }
        char prev[4096]; if (!getcwd(prev, sizeof(prev))) prev[0] = 0;
        if (chdir(dir) != 0) { fprintf(stderr, "arxsh: cd: %s: %s\n", dir, strerror(errno)); return 1; }
        if (prev[0]) setenv("OLDPWD", prev, 1);
        char now[4096]; if (getcwd(now, sizeof(now))) setenv("PWD", now, 1);
        if (ac > 1 && strcmp(a[1], "-") == 0) printf("%s\n", now);
        return 0;
    }
    if (strcmp(name, "help") == 0) {
        printf("arxsh " ARXSH_VERSION " — a shell written from scratch in C\n"
               "builtins: cd, exit, help, export, unset, alias, unalias, jobs, fg, bg, history\n"
               "features: pipes, redirection (< > >>), background jobs (&), quotes, $VAR and $? expansion,\n"
               "          ~ expansion, globbing, aliases, ~/.arxshrc, history with arrow keys, Ctrl-A/E/U/K/W/L\n"
               "while typing: ghost text completes from your history (Right arrow accepts it), the command\n"
               "          name glows red when arxsh cannot run it, and a mistyped command gets a did-you-mean.\n"
               "          Type a folder name on its own to cd into it. Commands slower than 2s report their time.\n");
        return 0;
    }
    if (strcmp(name, "export") == 0) {
        if (ac == 1) { for (char **e = environ; *e; e++) printf("%s\n", *e); return 0; }
        for (int i = 1; i < ac; i++) {
            char *eq = strchr(a[i], '=');
            if (eq) { *eq = 0; setenv(a[i], eq + 1, 1); *eq = '='; }
            else setenv(a[i], "", 0);
        }
        return 0;
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < ac; i++) unsetenv(a[i]);
        return 0;
    }
    if (strcmp(name, "alias") == 0) {
        if (ac == 1) {
            for (int i = 0; i < alias_count; i++) printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
            return 0;
        }
        int rc = 0;
        for (int i = 1; i < ac; i++) {
            char *eq = strchr(a[i], '=');
            if (eq) { *eq = 0; alias_set(a[i], eq + 1); *eq = '='; }
            else {
                const char *v = alias_get(a[i]);
                if (v) printf("alias %s='%s'\n", a[i], v);
                else { fprintf(stderr, "arxsh: alias: %s: not found\n", a[i]); rc = 1; }
            }
        }
        return rc;
    }
    if (strcmp(name, "unalias") == 0) {
        int rc = 0;
        for (int i = 1; i < ac; i++) if (alias_unset(a[i])) { fprintf(stderr, "arxsh: unalias: %s: not found\n", a[i]); rc = 1; }
        return rc;
    }
    if (strcmp(name, "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!jobs[i].used) continue;
            printf("[%d] %s\t\t%s\n", jobs[i].id, jobs[i].stopped ? "Stopped" : "Running", jobs[i].cmdline);
        }
        return 0;
    }
    if (strcmp(name, "history") == 0) {
        for (int i = 0; i < history_count; i++) printf("%5d  %s\n", i + 1, history_list[i]);
        return 0;
    }
    if (strcmp(name, "fg") == 0 || strcmp(name, "bg") == 0) {
        job_t *j = job_from_arg(ac > 1 ? a[1] : NULL);
        if (!j) { fprintf(stderr, "arxsh: %s: no such job\n", name); return 1; }
        int bgonly = (strcmp(name, "bg") == 0);
        printf("%s\n", j->cmdline);
        if (kill(-j->pgid, SIGCONT) < 0) kill(j->pids[0], SIGCONT);
        j->stopped = 0;
        if (bgonly) { printf("[%d] continued\t\t%s\n", j->id, j->cmdline); return 0; }
        if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, j->pgid);
        pid_t pids[MAX_CMDS]; memcpy(pids, j->pids, sizeof(pid_t) * j->npids);
        int np = j->npids, status = 0;
        int stopped = wait_for_pids(pids, np, &status);
        if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, shell_pgid);
        if (stopped) { j->stopped = 1; printf("[%d] Stopped\t\t%s\n", j->id, j->cmdline); return 128 + SIGTSTP; }
        last_status = status;
        for (int k = 0; k < j->npids; k++) reap_drop(j->pids[k]);
        job_remove(j);
        return status;
    }
    return 1;
}

/* ---------- executor ---------- */
static int apply_redirs(cmd_t *cmd) {
    if (cmd->in_path) {
        int fd = open(cmd->in_path, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "arxsh: %s: %s\n", cmd->in_path, strerror(errno)); return -1; }
        dup2(fd, STDIN_FILENO); close(fd);
    }
    if (cmd->out_path) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->out_path, flags, 0644);
        if (fd < 0) { fprintf(stderr, "arxsh: %s: %s\n", cmd->out_path, strerror(errno)); return -1; }
        dup2(fd, STDOUT_FILENO); close(fd);
    }
    return 0;
}

static void execute_pipeline(cmd_t *cmds, int nc, int background, const char *cmdline) {
    struct timespec t0;
    int timing = (!background && isatty(STDIN_FILENO));
    if (timing) clock_gettime(CLOCK_MONOTONIC, &t0);
    /* single builtin, run in-process so cd/export/alias stick */
    if (nc == 1 && is_builtin(cmds[0].argv[0])) {
        int saved_in = -1, saved_out = -1, rc;
        if (cmds[0].in_path || cmds[0].out_path) {
            saved_in = dup(STDIN_FILENO); saved_out = dup(STDOUT_FILENO);
            if (apply_redirs(&cmds[0]) != 0) {
                if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
                if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
                last_status = 1; return;
            }
        }
        rc = run_builtin(&cmds[0]);
        fflush(stdout);
        if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        last_status = rc;
        return;
    }

    int pipes[MAX_CMDS - 1][2];
    for (int i = 0; i < nc - 1; i++) if (pipe(pipes[i]) < 0) { perror("arxsh: pipe"); last_status = 1; return; }

    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;
    for (int i = 0; i < nc; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            /* partial pipeline failure: kill what already started, reap it, bail */
            perror("arxsh: fork");
            if (pgid > 0) {
                kill(-pgid, SIGKILL);
                int dummy = 0; (void)wait_for_pids(pids, i, &dummy);
                for (int k = 0; k < i; k++) reap_drop(pids[k]);
            }
            for (int k = 0; k < nc - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            last_status = 1;
            return;
        }
        if (pid == 0) {
            /* child */
            if (pgid == 0) setpgid(0, 0); else setpgid(0, pgid);
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < nc - 1) dup2(pipes[i][1], STDOUT_FILENO);
            for (int k = 0; k < nc - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            if (apply_redirs(&cmds[i]) != 0) _exit(1);
            if (is_builtin(cmds[i].argv[0])) { int brc = run_builtin(&cmds[i]); fflush(stdout); _exit(brc); }
            execvp(cmds[i].argv[0], cmds[i].argv);
            if (errno == ENOENT) {
                fprintf(stderr, "arxsh: command not found: %s\n", cmds[i].argv[0]);
                char *sug = did_you_mean(cmds[i].argv[0]);
                if (sug) { fprintf(stderr, "arxsh: did you mean '%s'?\n", sug); free(sug); }
            }
            else fprintf(stderr, "arxsh: %s: %s\n", cmds[i].argv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
        reap_watch(pid);
    }
    for (int k = 0; k < nc - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }

    if (background) {
        job_t *j = job_add(pgid, pids, nc, cmdline, 0);
        if (j) printf("[%d] %d\n", j->id, (int)pgid);
        last_status = 0;
        return;
    }
    if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, pgid);
    int status = 0;
    pid_t waitpids[MAX_CMDS]; memcpy(waitpids, pids, sizeof(pid_t) * nc);
    int stopped = wait_for_pids(waitpids, nc, &status);
    if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (stopped) {
        job_t *j = job_add(pgid, pids, nc, cmdline, 1);
        if (j) printf("\n[%d] Stopped\t\t%s\n", j->id, j->cmdline);
        last_status = 128 + SIGTSTP;
    } else {
        last_status = status;
        for (int k = 0; k < nc; k++) reap_drop(pids[k]);
    }
    if (timing) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= 2.0) printf("\033[2m  took %.1fs\033[0m\n", el);
    }
}

/* expand a leading alias (textual, once per name) and execute the line */
static int execute_line(char *line) {
    char *expanded = line;
    char used_names[MAX_ALIASES][64]; int used_count = 0;
    for (int guard = 0; guard < 16; guard++) {
        /* find the first word */
        const char *p = expanded;
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '&' && *p != ';') p++;
        if (p == s) break;
        char name[64]; size_t n = (size_t)(p - s); if (n > 63) n = 63;
        memcpy(name, s, n); name[n] = 0;
        const char *val = alias_get(name);
        if (!val) break;
        int seen = 0;
        for (int i = 0; i < used_count; i++) if (strcmp(used_names[i], name) == 0) seen = 1;
        if (seen) break;
        strcpy(used_names[used_count++], name);
        sb_t nb; sb_init(&nb);
        sb_puts(&nb, val);
        sb_puts(&nb, p);
        if (expanded != line) free(expanded);
        expanded = nb.buf;
    }

    token_t *toks = tokenize(expanded);
    cmd_t cmds[MAX_CMDS];
    int background = 0;
    int nc = parse_tokens(toks, cmds, &background);
    /* auto-cd: a lone word that names a folder (and not a runnable command) cds into it */
    if (nc == 1 && !background && cmds[0].argc == 1 && !cmds[0].in_path && !cmds[0].out_path
        && !is_builtin(cmds[0].argv[0]) && !alias_get(cmds[0].argv[0])) {
        const char *target = cmds[0].argv[0];
        struct stat st;
        int known_cmd = (!strchr(target, '/') && path_lookup(target));
        if (!known_cmd && stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
            char prev[4096]; if (!getcwd(prev, sizeof(prev))) prev[0] = 0;
            if (chdir(target) == 0) {
                if (prev[0]) setenv("OLDPWD", prev, 1);
                char now[4096]; if (getcwd(now, sizeof(now))) setenv("PWD", now, 1);
                last_status = 0;
            } else {
                fprintf(stderr, "arxsh: cd: %s: %s\n", target, strerror(errno));
                last_status = 1;
            }
            cmds_free(cmds, nc);
            tokens_free(toks);
            int rc0 = last_status;
            if (expanded != line) free(expanded);
            return rc0;
        }
    }
    if (nc > 0) execute_pipeline(cmds, nc, background, expanded);
    else if (nc < 0) last_status = 2;
    if (nc > 0) cmds_free(cmds, nc);
    tokens_free(toks);
    int rc = last_status;
    if (expanded != line) free(expanded);
    return rc;
}

static void run_rc(void) {
    const char *home = getenv("HOME"); if (!home) return;
    char path[4352]; snprintf(path, sizeof(path), "%s/.arxshrc", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        while (n > 0 && line[n - 1] == '\n') line[--n] = 0;
        if (n == 0) continue;
        execute_line(line);
    }
    free(line); fclose(f);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    shell_pgid = getpid();
    if (isatty(STDIN_FILENO)) {
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcgetattr(STDIN_FILENO, &orig_termios);
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    history_load();
    run_rc();

    int interactive = isatty(STDIN_FILENO);
    if (interactive)
        printf("\033[2marxsh " ARXSH_VERSION " — ghost suggestions, did-you-mean, auto-cd. 'help' lists the builtins.\033[0m\n");
    for (;;) {
        jobs_sweep();
        char *line;
        if (interactive) {
            sb_t prompt; sb_init(&prompt);
            build_prompt(&prompt);
            line = read_line_tty(prompt.buf);
            sb_free(&prompt);
            if (!line) { printf("\n"); break; }   /* Ctrl-D */
        } else {
            line = NULL; size_t cap = 0;
            if (getline(&line, &cap, stdin) < 0) { free(line); break; }
            size_t len = strlen(line);
            while (len > 0 && line[len - 1] == '\n') line[--len] = 0;
        }
        if (!line) break;
        if (line[0]) {
            history_add(line);
            char *copy = xstrdup(line);
            execute_line(copy);
            free(copy);
        }
        free(line);
    }
    history_save();
    return last_status;
}
