/*
 * hegl.c -- "heg-lite": the kernel-portable core of heg shell.
 *
 * Freestanding by design: no stdio, no malloc, no string.h, no signals, no
 * environment. Every OS interaction goes through hegl_sys.h (12 operations).
 * All memory is static, so it also runs where there is no heap yet.
 *
 * Supports: pipelines (a | b | c), redirection (< > >>), background (&),
 *           sequencing (;), quotes ('..' ".."), PATH lookup,
 *           builtins: cd pwd exit help
 *
 * Build on Linux/macOS:   cc -std=c99 -Wall -Wextra -O2 -o hegl hegl.c
 * Build on xv6:           add -DHEGL_XV6 and put it in the user/ directory.
 * See KERNEL_PORT.md.
 */
#include "hegl_sys.h"

#define MAXLINE 512
#define MAXTOK 64
#define MAXARG 32
#define MAXSTAGE 8

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APP, T_BG, T_SEMI };

typedef struct {
    char *argv[MAXARG];
    char *in, *out;
    int append;
} Stage;

/* ---- tiny libc replacement ---- */
static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int has_slash(const char *s) {
    for (; *s; s++) if (*s == '/') return 1;
    return 0;
}
static void put(int fd, const char *s) { hs_write(fd, s, slen(s)); }
static void complain(const char *a, const char *b) {
    put(2, "heg: "); put(2, a);
    if (b) { put(2, ": "); put(2, b); }
    put(2, "\n");
}

/* ---- lexer: line -> tokens (words are copied into wbuf, quotes removed) ---- */
static char wbuf[MAXLINE + MAXTOK];
static int ttype[MAXTOK];
static char *tword[MAXTOK];
static int ntok;

static int is_op(char c) { return c == '|' || c == '<' || c == '>' || c == '&' || c == ';'; }
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int lex(const char *p) {
    char *w = wbuf;
    ntok = 0;
    while (*p) {
        if (is_sp(*p)) { p++; continue; }
        if (ntok >= MAXTOK - 1) { complain("too many tokens", 0); return -1; }
        if (*p == '#') break;
        if (is_op(*p)) {
            int t = *p == '|' ? T_PIPE : *p == '<' ? T_IN : *p == '&' ? T_BG : *p == ';' ? T_SEMI : T_OUT;
            if (*p == '>' && p[1] == '>') { t = T_APP; p++; }
            ttype[ntok] = t; tword[ntok++] = 0; p++;
            continue;
        }
        ttype[ntok] = T_WORD; tword[ntok++] = w;
        while (*p && !is_sp(*p) && !is_op(*p)) {
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                while (*p && *p != q) *w++ = *p++;
                if (!*p) { complain("unterminated quote", 0); return -1; }
                p++;
            } else {
                *w++ = *p++;
            }
        }
        *w++ = 0;
    }
    return 0;
}

/* ---- execution ---- */
static char cwdbuf[256];

static void exec_search(char *const argv[]) {
    if (has_slash(argv[0])) { hs_exec(argv[0], argv); return; }
    const char *path = hs_default_path();
    char full[256];
    while (*path) {
        int n = 0;
        while (*path && *path != ':' && n < 200) full[n++] = *path++;
        while (*path && *path != ':') path++;
        if (*path == ':') path++;
        if (n && full[n - 1] != '/') full[n++] = '/';
        for (const char *c = argv[0]; *c && n < 250; ) full[n++] = *c++;
        full[n] = 0;
        hs_exec(full, argv);
    }
}

static int builtin(Stage *s, int *status) {
    char **a = s->argv;
    if (seq(a[0], "exit")) {
        int code = 0;
        if (a[1]) for (const char *c = a[1]; *c >= '0' && *c <= '9'; c++) code = code * 10 + (*c - '0');
        hs_exit(code);
    }
    if (seq(a[0], "cd")) {
        const char *d = a[1] ? a[1] : "/";
        if (hs_chdir(d) < 0) { complain("cd: no such directory", d); *status = 1; }
        else *status = 0;
        return 1;
    }
    if (seq(a[0], "pwd")) {
        if (hs_getcwd(cwdbuf, sizeof cwdbuf) < 0) { complain("pwd: not supported by this kernel", 0); *status = 1; }
        else { put(1, cwdbuf); put(1, "\n"); *status = 0; }
        return 1;
    }
    if (seq(a[0], "help")) {
        put(1, "heg-lite: pipes |, redirects < > >>, background &, ';', quotes.\nbuiltins: cd pwd exit help\n");
        *status = 0;
        return 1;
    }
    return 0;
}

static int run_pipeline(Stage *st, int n, int bg) {
    int pids[MAXSTAGE], prev = -1, status = 0;
    for (int i = 0; i < n; i++) {
        int p[2] = {-1, -1};
        if (i < n - 1 && hs_pipe(p) < 0) { complain("pipe failed", 0); return 1; }
        int pid = hs_fork();
        if (pid < 0) { complain("fork failed", 0); return 1; }
        if (pid == 0) {
            if (prev >= 0) { hs_dup_to(prev, 0); hs_close(prev); }
            if (p[1] >= 0) { hs_dup_to(p[1], 1); hs_close(p[0]); hs_close(p[1]); }
            if (st[i].in) {
                int fd = hs_open(st[i].in, HS_RDONLY);
                if (fd < 0) { complain(st[i].in, "cannot open for reading"); hs_exit(1); }
                hs_dup_to(fd, 0); if (fd != 0) hs_close(fd);
            }
            if (st[i].out) {
                int fd = hs_open(st[i].out, st[i].append ? HS_APPEND : HS_TRUNC);
                if (fd < 0) { complain(st[i].out, "cannot open for writing"); hs_exit(1); }
                hs_dup_to(fd, 1); if (fd != 1) hs_close(fd);
            }
            exec_search(st[i].argv);
            complain(st[i].argv[0], "command not found");
            hs_exit(127);
        }
        pids[i] = pid;
        if (prev >= 0) hs_close(prev);
        if (p[1] >= 0) hs_close(p[1]);
        prev = p[0];
    }
    if (bg) { put(1, "[background]\n"); return 0; }
    int left = n;
    while (left > 0) {
        int st_code, pid = hs_wait(&st_code);
        if (pid < 0) break;
        for (int i = 0; i < n; i++)
            if (pids[i] == pid) { left--; if (i == n - 1) status = st_code; }
    }
    return status;
}

/* run tokens [a, b) as one pipeline */
static int run_tokens(int a, int b, int bg) {
    static Stage st[MAXSTAGE];
    int n = 0, argc = 0;
    if (a == b) return 0;
    for (int i = 0; i < MAXSTAGE; i++) { st[i].argv[0] = 0; st[i].in = st[i].out = 0; st[i].append = 0; }
    for (int i = a; i < b; i++) {
        Stage *s = &st[n];
        switch (ttype[i]) {
        case T_WORD:
            if (argc >= MAXARG - 1) { complain("too many arguments", 0); return 1; }
            s->argv[argc++] = tword[i]; s->argv[argc] = 0;
            break;
        case T_IN: case T_OUT: case T_APP:
            if (i + 1 >= b || ttype[i + 1] != T_WORD) { complain("syntax error: missing file after redirect", 0); return 2; }
            if (ttype[i] == T_IN) s->in = tword[i + 1];
            else { s->out = tword[i + 1]; s->append = ttype[i] == T_APP; }
            i++;
            break;
        case T_PIPE:
            if (!s->argv[0]) { complain("syntax error: empty command before |", 0); return 2; }
            if (++n >= MAXSTAGE) { complain("pipeline too long", 0); return 1; }
            argc = 0;
            break;
        default:
            complain("syntax error", 0); return 2;
        }
    }
    if (!st[n].argv[0]) { complain("syntax error: empty command", 0); return 2; }
    n++;
    int status;
    if (n == 1 && !bg && builtin(&st[0], &status)) return status;
    return run_pipeline(st, n, bg);
}

static int run_line(const char *line) {
    int status = 0, start = 0;
    if (lex(line) < 0) return 1;
    for (int i = 0; i <= ntok; i++) {
        if (i == ntok || ttype[i] == T_SEMI || ttype[i] == T_BG) {
            status = run_tokens(start, i, i < ntok && ttype[i] == T_BG);
            start = i + 1;
        }
    }
    return status;
}

/* ---- main loop ---- */
int main(void) {
    static char line[MAXLINE];
    int tty = hs_isatty(0), status = 0;
    for (;;) {
        int n = 0, eof = 0, overflow = 0;
        if (tty) put(1, "heg> ");
        for (;;) {
            char c;
            int r = hs_read(0, &c, 1);
            if (r <= 0) { eof = 1; break; }
            if (c == '\n') break;
            if (n < MAXLINE - 1) line[n++] = c; else overflow = 1;
        }
        line[n] = 0;
        if (overflow) complain("line too long", 0);
        else if (n) status = run_line(line);
        if (eof) {
            if (tty) put(1, "\n");
            hs_exit(status);
        }
    }
}
