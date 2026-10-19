/*
 * heg -- a small POSIX-style Unix shell written in C.
 *
 * Features
 *   - pipelines (|), lists (;  &&  ||), background jobs (&)
 *   - ordered redirections: <  >  >>  N>  N>>  N<  N>&M   (e.g. 2>&1)
 *   - quoting ('..', "..", \x), $VAR ${VAR} $? $$ $!, ~ and * ? [..] globbing
 *   - job control: process groups, Ctrl-C / Ctrl-Z, jobs, fg, bg, kill, wait
 *   - builtins: cd pwd echo export unset exit history source help + job builtins
 *   - command history (persisted in ~/.heg_history); optional readline support
 *
 * Expansion happens at execution time, per pipeline, so that
 * `cd /tmp && echo $PWD` and `false; echo $?` behave like they do in bash.
 *
 * Layout of this file:
 *   1. utilities        2. tokenizer       3. expansion      4. parser
 *   5. job control      6. builtins        7. executor       8. REPL / main
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>

#define HEG_VERSION "0.2.0"
#define MAX_PROCS 32 /* max commands in one pipeline */
#define MAX_JOBS 64
#define MAX_HISTORY 1000

extern char **environ;

/* ------------------------------------------------------------------ */
/* 1. utilities and global state                                       */
/* ------------------------------------------------------------------ */

static int interactive;              /* reading from a terminal, job control on */
static int shell_tty = STDIN_FILENO;
static pid_t shell_pgid;
static struct termios shell_tmodes;
static int last_status;              /* $? */
static pid_t last_bg_pid;            /* $! */
static int should_exit;
static int exit_code;
static int line_cancelled;           /* the line editor ended with Ctrl-C */
static int job_control;              /* we own the terminal and use process groups */
enum { CTL_NONE, CTL_BREAK, CTL_CONTINUE, CTL_RETURN };
static int ctl, ctl_n;               /* pending break / continue / return */
static char **pos_args;              /* $1 $2 ... */
static int npos;
static const char *shell_name = "heg"; /* $0 */

static char *history[MAX_HISTORY];
static int nhistory;

static void run_line(const char *line);
static void run_stream(FILE *f);
static int skip_cmdsub(const char *s, size_t *pi);
static int is_assignment(const char *s);
struct Func;
static struct Func *find_func(const char *name);
static int bi_type(char **argv);
static char *expand_word(const char *s, int *glob, int *quoted, char **maskp);

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        perror("heg: malloc");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        perror("heg: realloc");
        exit(1);
    }
    return q;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

static void warn_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("heg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* growable string buffer */
typedef struct {
    char *s;
    size_t len, cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 32;
    b->len = 0;
    b->s = xmalloc(b->cap);
    b->s[0] = '\0';
}

static void buf_putc(Buf *b, char c) {
    if (b->len + 1 >= b->cap) {
        b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) {
    while (*s) buf_putc(b, *s++);
}

/* ------------------------------------------------------------------ */
/* 2. tokenizer                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    T_WORD, T_PIPE, T_AMP, T_SEMI, T_AND, T_OR,
    T_LT, T_GT, T_GTGT, T_DUP, T_AMPGT, T_AMPGTGT,
    T_NL, T_LPAREN, T_RPAREN, T_HEREDOC, T_EOF
} TokType;

typedef struct {
    TokType type;
    char *text; /* raw source text of a word (quotes still present) */
    int fd;     /* redirection: the fd being redirected */
    int dup;    /* T_DUP: the fd it is duplicated from; T_HEREDOC: flags (1 strip tabs, 2 quoted) */
    char *body; /* T_HEREDOC: the collected body text */
} Token;

typedef struct {
    Token *v;
    int n, cap;
} TokList;

static void tok_push(TokList *t, TokType type, char *text) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    t->v[t->n].type = type;
    t->v[t->n].text = text;
    t->v[t->n].fd = -1;
    t->v[t->n].dup = -1;
    t->v[t->n].body = NULL;
    t->n++;
}

static void free_tokens(TokList *t) {
    for (int i = 0; i < t->n; i++) {
        free(t->v[i].text);
        free(t->v[i].body);
    }
    free(t->v);
    t->v = NULL;
    t->n = t->cap = 0;
}

static int is_op_char(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

/*
 * Split text into tokens. Words are kept RAW (quotes and $ intact); they are
 * expanded later, right before the command runs.
 *
 * Returns 0 on success, -1 on a syntax error, TOK_INCOMPLETE when the input
 * ends inside a quote, after a trailing backslash, or before a here-document
 * body is complete (the caller then reads another line and retries).
 */
#define TOK_INCOMPLETE 1

/* Scan a here-document delimiter word starting at s[*pk]; strips quotes. */
static int scan_heredoc_delim(const char *s, size_t *pk, Buf *d, int *quoted) {
    size_t k = *pk;
    while (s[k] && !isspace((unsigned char)s[k]) && !is_op_char(s[k])) {
        if (s[k] == '\'' || s[k] == '"') {
            char q = s[k++];
            *quoted = 1;
            while (s[k] && s[k] != q) buf_putc(d, s[k++]);
            if (!s[k]) return TOK_INCOMPLETE;
            k++;
        } else if (s[k] == '\\') {
            *quoted = 1;
            k++;
            if (s[k]) buf_putc(d, s[k++]);
        } else {
            buf_putc(d, s[k++]);
        }
    }
    *pk = k;
    return 0;
}

/* After a newline: read the bodies of the pending here-documents. */
static int read_heredoc_bodies(const char *s, size_t *pi, TokList *t, const int *pend, int npend) {
    size_t i = *pi;
    for (int h = 0; h < npend; h++) {
        Token *tk = &t->v[pend[h]];
        size_t dl = strlen(tk->text);
        Buf body;
        int found = 0;
        buf_init(&body);
        while (s[i]) {
            size_t ls = i;
            while (s[i] && s[i] != '\n') i++;
            const char *lp = s + ls;
            size_t ll = i - ls;
            if (s[i]) i++;
            if (tk->dup & 1) { /* <<- : strip leading tabs */
                while (ll && *lp == '\t') {
                    lp++;
                    ll--;
                }
            }
            if (ll == dl && strncmp(lp, tk->text, dl) == 0) {
                found = 1;
                break;
            }
            for (size_t k = 0; k < ll; k++) buf_putc(&body, lp[k]);
            buf_putc(&body, '\n');
        }
        if (!found) {
            free(body.s);
            return TOK_INCOMPLETE;
        }
        tk->body = body.s;
    }
    *pi = i;
    return 0;
}

static int tokenize(const char *s, TokList *t) {
    size_t i = 0;
    int pend[16], npend = 0; /* here-document tokens waiting for their bodies */
    int rc;
    memset(t, 0, sizeof *t);

    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            tok_push(t, T_NL, NULL);
            i++;
            if (npend && (rc = read_heredoc_bodies(s, &i, t, pend, npend)) != 0) goto fail;
            npend = 0;
            continue;
        }
        if (isspace(c)) {
            i++;
            continue;
        }
        if (c == '#') { /* comment to end of line */
            while (s[i] && s[i] != '\n') i++;
            continue;
        }
        if (c == '(' || c == ')') {
            tok_push(t, c == '(' ? T_LPAREN : T_RPAREN, NULL);
            i++;
            continue;
        }

        /* redirections, with an optional single-digit fd prefix: 2>file */
        {
            int fd = -1;
            size_t j = i;
            if (isdigit(c) && (s[i + 1] == '<' || s[i + 1] == '>') &&
                (i == 0 || isspace((unsigned char)s[i - 1]))) {
                fd = s[i] - '0';
                j = i + 1;
            }
            if (s[j] == '<' || s[j] == '>') {
                if (s[j] == '<' && s[j + 1] == '<') { /* here-document */
                    size_t k = j + 2;
                    int strip = 0, quoted = 0;
                    Buf d;
                    if (s[k] == '-') {
                        strip = 1;
                        k++;
                    }
                    while (s[k] == ' ' || s[k] == '\t') k++;
                    buf_init(&d);
                    if ((rc = scan_heredoc_delim(s, &k, &d, &quoted)) != 0) {
                        free(d.s);
                        goto fail;
                    }
                    if (d.len == 0 || npend >= 16) {
                        warn_msg("syntax error: bad here-document");
                        free(d.s);
                        rc = -1;
                        goto fail;
                    }
                    tok_push(t, T_HEREDOC, d.s);
                    t->v[t->n - 1].fd = fd < 0 ? 0 : fd;
                    t->v[t->n - 1].dup = strip | (quoted ? 2 : 0);
                    pend[npend++] = t->n - 1;
                    i = k;
                    continue;
                }
                if (s[j] == '<') {
                    tok_push(t, T_LT, NULL);
                    t->v[t->n - 1].fd = fd < 0 ? 0 : fd;
                    i = j + 1;
                } else if (s[j + 1] == '>') {
                    tok_push(t, T_GTGT, NULL);
                    t->v[t->n - 1].fd = fd < 0 ? 1 : fd;
                    i = j + 2;
                } else if (s[j + 1] == '&' && isdigit((unsigned char)s[j + 2])) {
                    tok_push(t, T_DUP, NULL);
                    t->v[t->n - 1].fd = fd < 0 ? 1 : fd;
                    t->v[t->n - 1].dup = s[j + 2] - '0';
                    i = j + 3;
                } else {
                    tok_push(t, T_GT, NULL);
                    t->v[t->n - 1].fd = fd < 0 ? 1 : fd;
                    i = j + 1;
                }
                continue;
            }
        }

        if (c == '&' && s[i + 1] == '>') { /* &> file  and  &>> file */
            int app = s[i + 2] == '>';
            tok_push(t, app ? T_AMPGTGT : T_AMPGT, NULL);
            i += app ? 3 : 2;
            continue;
        }
        if (c == '|') {
            if (s[i + 1] == '|') {
                tok_push(t, T_OR, NULL);
                i += 2;
            } else {
                tok_push(t, T_PIPE, NULL);
                i++;
            }
            continue;
        }
        if (c == '&') {
            if (s[i + 1] == '&') {
                tok_push(t, T_AND, NULL);
                i += 2;
            } else {
                tok_push(t, T_AMP, NULL);
                i++;
            }
            continue;
        }
        if (c == ';') {
            tok_push(t, T_SEMI, NULL);
            i++;
            continue;
        }

        /* a word: scan to its end, honouring quotes, $(...) and `...` */
        size_t start = i;
        while (s[i] && !isspace((unsigned char)s[i]) && !is_op_char(s[i])) {
            if (s[i] == '\\') {
                i++;
                if (!s[i]) goto incomplete;
                i++;
            } else if (s[i] == '\'') {
                i++;
                while (s[i] && s[i] != '\'') i++;
                if (!s[i]) goto incomplete;
                i++;
            } else if (s[i] == '"') {
                i++;
                while (s[i] && s[i] != '"') {
                    if (s[i] == '\\' && s[i + 1]) {
                        i += 2;
                    } else if (s[i] == '$' && s[i + 1] == '(') {
                        size_t j = i;
                        if (skip_cmdsub(s, &j) < 0) goto incomplete;
                        i = j;
                    } else if (s[i] == '`') {
                        i++;
                        while (s[i] && s[i] != '`') {
                            if (s[i] == '\\' && s[i + 1]) i++;
                            i++;
                        }
                        if (!s[i]) goto incomplete;
                        i++;
                    } else {
                        i++;
                    }
                }
                if (!s[i]) goto incomplete;
                i++;
            } else if (s[i] == '$' && s[i + 1] == '(') {
                size_t j = i;
                if (skip_cmdsub(s, &j) < 0) goto incomplete;
                i = j;
            } else if (s[i] == '`') {
                i++;
                while (s[i] && s[i] != '`') {
                    if (s[i] == '\\' && s[i + 1]) i++;
                    i++;
                }
                if (!s[i]) goto incomplete;
                i++;
            } else {
                i++;
            }
        }
        tok_push(t, T_WORD, xstrndup(s + start, i - start));
    }
    if (npend) goto incomplete; /* here-document body not started yet */
    tok_push(t, T_EOF, NULL);
    return 0;

incomplete:
    rc = TOK_INCOMPLETE;
fail:
    free_tokens(t);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 3. expansion ($vars, $(cmd), quotes, ~, globs) -- at execution time */
/* ------------------------------------------------------------------ */

/* Expanded text plus a parallel mask: '1' marks characters that came from an
 * UNQUOTED expansion and are therefore subject to field splitting. */
typedef struct {
    Buf b, m;
} Word;

static void word_init(Word *w) {
    buf_init(&w->b);
    buf_init(&w->m);
}

static void word_putc(Word *w, char c, int sp) {
    buf_putc(&w->b, c);
    buf_putc(&w->m, sp ? '1' : '0');
}

static void word_puts(Word *w, const char *s, int sp) {
    while (*s) word_putc(w, *s++, sp);
}

/* s[*pi..] starts with "$(": advance *pi just past the matching ')'. */
static int skip_cmdsub(const char *s, size_t *pi) {
    size_t i = *pi + 2;
    int depth = 1;
    while (s[i] && depth > 0) {
        if (s[i] == '\\' && s[i + 1]) {
            i += 2;
            continue;
        }
        if (s[i] == '\'') {
            i++;
            while (s[i] && s[i] != '\'') i++;
            if (!s[i]) return -1;
            i++;
            continue;
        }
        if (s[i] == '"') {
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\' && s[i + 1]) i += 2;
                else i++;
            }
            if (!s[i]) return -1;
            i++;
            continue;
        }
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        i++;
    }
    if (depth > 0) return -1;
    *pi = i;
    return 0;
}

/*
 * Run `cmd` in a forked child of this shell with stdout connected to a pipe,
 * and return everything it printed (trailing newlines stripped).
 */
static char *command_substitute(const char *cmd) {
    int fd[2];
    Buf out;
    buf_init(&out);
    if (pipe(fd) < 0) return out.s;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return out.s;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        interactive = 0; /* the child is not a job-control shell */
        job_control = 0;
        run_line(cmd);
        fflush(NULL);
        _exit(should_exit ? exit_code : last_status);
    }
    close(fd[1]);
    char tmp[4096];
    ssize_t n;
    while ((n = read(fd[0], tmp, sizeof tmp)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (ssize_t k = 0; k < n; k++) buf_putc(&out, tmp[k]);
    }
    close(fd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    while (out.len > 0 && out.s[out.len - 1] == '\n') out.s[--out.len] = '\0';
    return out.s;
}

/* s[*pi] == '`': run the backquoted command and append its output. */
static void expand_backtick(const char *s, size_t *pi, Word *w, int sp) {
    size_t j = *pi + 1;
    while (s[j] && s[j] != '`') {
        if (s[j] == '\\' && s[j + 1]) j++;
        j++;
    }
    char *inner = xstrndup(s + *pi + 1, j - *pi - 1);
    char *out = command_substitute(inner);
    word_puts(w, out, sp);
    free(out);
    free(inner);
    *pi = s[j] ? j + 1 : j;
}

/* ---- $(( )) integer arithmetic: precedence climbing ---- */

typedef struct {
    const char *p;
    int err; /* 1 = syntax error, 2 = division by zero */
} Arith;

static long arith_expr(Arith *a, int minprec);

static void arith_ws(Arith *a) {
    while (isspace((unsigned char)*a->p)) a->p++;
}

static long arith_primary(Arith *a) {
    arith_ws(a);
    char c = *a->p;
    if (c == '(') {
        a->p++;
        long v = arith_expr(a, 0);
        arith_ws(a);
        if (*a->p == ')') a->p++;
        else a->err = 1;
        return v;
    }
    if (c == '-') {
        a->p++;
        return -arith_primary(a);
    }
    if (c == '+') {
        a->p++;
        return arith_primary(a);
    }
    if (c == '!') {
        a->p++;
        return !arith_primary(a);
    }
    if (isdigit((unsigned char)c)) {
        char *e;
        long v = strtol(a->p, &e, 0);
        a->p = e;
        return v;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        char name[128];
        size_t k = 0;
        while ((isalnum((unsigned char)*a->p) || *a->p == '_') && k < sizeof name - 1)
            name[k++] = *a->p++;
        name[k] = '\0';
        const char *v = getenv(name);
        return v ? strtol(v, NULL, 0) : 0;
    }
    a->err = 1;
    return 0;
}

static long arith_expr(Arith *a, int minprec) {
    static const struct {
        const char *s;
        int prec;
    } ops[] = {{"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<=", 4}, {">=", 4},
               {"<", 4},  {">", 4},  {"+", 5},  {"-", 5},  {"*", 6},  {"/", 6},
               {"%", 6},  {NULL, 0}};
    long lhs = arith_primary(a);
    for (;;) {
        arith_ws(a);
        int oi = -1;
        for (int i = 0; ops[i].s; i++) {
            if (strncmp(a->p, ops[i].s, strlen(ops[i].s)) == 0) {
                oi = i;
                break;
            }
        }
        if (oi < 0 || ops[oi].prec < minprec) return lhs;
        const char *op = ops[oi].s;
        a->p += strlen(op);
        long rhs = arith_expr(a, ops[oi].prec + 1);
        if (!strcmp(op, "||")) lhs = lhs || rhs;
        else if (!strcmp(op, "&&")) lhs = lhs && rhs;
        else if (!strcmp(op, "==")) lhs = lhs == rhs;
        else if (!strcmp(op, "!=")) lhs = lhs != rhs;
        else if (!strcmp(op, "<=")) lhs = lhs <= rhs;
        else if (!strcmp(op, ">=")) lhs = lhs >= rhs;
        else if (!strcmp(op, "<")) lhs = lhs < rhs;
        else if (!strcmp(op, ">")) lhs = lhs > rhs;
        else if (!strcmp(op, "+")) lhs = lhs + rhs;
        else if (!strcmp(op, "-")) lhs = lhs - rhs;
        else if (!strcmp(op, "*")) lhs = lhs * rhs;
        else if (rhs == 0) a->err = 2;
        else if (!strcmp(op, "/")) lhs = lhs / rhs;
        else lhs = lhs % rhs;
    }
}

static long arith_eval(const char *expr) {
    Arith a = {expr, 0};
    long v = arith_expr(&a, 0);
    arith_ws(&a);
    if (*a.p) a.err = a.err ? a.err : 1;
    if (a.err == 2) warn_msg("arithmetic: division by zero");
    else if (a.err) warn_msg("arithmetic: syntax error in expression `%s'", expr);
    return a.err ? 0 : v;
}

/* value of a named variable or positional parameter ($0 $1 ...), or NULL if unset */
static const char *lookup_var(const char *name) {
    if (isdigit((unsigned char)name[0])) {
        int n = atoi(name);
        if (n == 0) return shell_name;
        return n <= npos ? pos_args[n - 1] : NULL;
    }
    return getenv(name);
}

/*
 * ${NAME} ${#NAME} ${NAME:-w} ${NAME-w} ${NAME:+w} ${NAME+w} ${NAME:=w}
 * s[*pi] == '$', s[*pi+1] == '{'. Returns -1 if it is not a valid expansion.
 */
static int expand_braced(const char *s, size_t *pi, Word *w, int sp) {
    size_t i = *pi + 2, j = i;
    int depth = 1;
    while (s[j]) {
        if (s[j] == '{') depth++;
        else if (s[j] == '}' && --depth == 0) break;
        j++;
    }
    if (s[j] != '}') return -1;

    char *body = xstrndup(s + i, j - i);
    const char *e = body;
    int want_len = 0;
    if (*e == '#' && e[1]) {
        want_len = 1;
        e++;
    }
    size_t nl = 0;
    while (isalnum((unsigned char)e[nl]) || e[nl] == '_') nl++;
    if (nl == 0) {
        free(body);
        return -1;
    }
    char *name = xstrndup(e, nl);
    const char *rest = e + nl;
    const char *val = lookup_var(name);

    if (want_len) {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%zu", val ? strlen(val) : (size_t)0);
        word_puts(w, tmp, sp);
    } else if (*rest == '\0') {
        if (val) word_puts(w, val, sp);
    } else {
        int colon = 0;
        char op = 0;
        if (*rest == ':' && rest[1] && strchr("-+=", rest[1])) {
            colon = 1;
            op = rest[1];
            rest += 2;
        } else if (strchr("-+=", *rest)) {
            op = *rest;
            rest += 1;
        }
        if (op) {
            int empty = !val || !*val;
            int use_word = op == '+' ? (colon ? !empty : val != NULL)
                                     : (colon ? empty : val == NULL);
            if (use_word) {
                int g, q;
                char *ex = expand_word(rest, &g, &q, NULL);
                if (op == '=') setenv(name, ex, 1);
                word_puts(w, ex, sp);
                free(ex);
            } else if (op != '+' && val) {
                word_puts(w, val, sp);
            }
        }
    }
    free(name);
    free(body);
    *pi = j + 1;
    return 0;
}

/* s[*pi] == '$'. Appends the expansion to w and advances *pi. */
static void expand_var(const char *s, size_t *pi, Word *w, int sp) {
    size_t i = *pi + 1;
    char name[128];
    size_t k = 0;
    char tmp[32];
    const char *v;

    if (s[i] == '(' && s[i + 1] == '(') { /* $(( expr )) */
        size_t j = *pi;
        if (skip_cmdsub(s, &j) == 0 && j - *pi >= 5 && s[j - 2] == ')') {
            char *inner = xstrndup(s + *pi + 3, j - *pi - 5);
            int g, q;
            char *ex = expand_word(inner, &g, &q, NULL);
            snprintf(tmp, sizeof tmp, "%ld", arith_eval(ex));
            word_puts(w, tmp, sp);
            free(ex);
            free(inner);
            *pi = j;
            return;
        }
    }
    if (s[i] == '(') {
        size_t j = *pi;
        if (skip_cmdsub(s, &j) == 0) {
            char *inner = xstrndup(s + *pi + 2, j - *pi - 3);
            char *out = command_substitute(inner);
            word_puts(w, out, sp);
            free(out);
            free(inner);
            *pi = j;
            return;
        }
    }
    if (isdigit((unsigned char)s[i])) { /* $0 $1 ... $9 */
        int d = s[i] - '0';
        if (d == 0) word_puts(w, shell_name, sp);
        else if (d <= npos) word_puts(w, pos_args[d - 1], sp);
        *pi = i + 1;
        return;
    }
    if (s[i] == '#') {
        snprintf(tmp, sizeof tmp, "%d", npos);
        word_puts(w, tmp, sp);
        *pi = i + 1;
        return;
    }
    if (s[i] == '@' || s[i] == '*') {
        for (int a = 0; a < npos; a++) {
            if (a) word_putc(w, ' ', sp);
            word_puts(w, pos_args[a], sp);
        }
        *pi = i + 1;
        return;
    }
    if (s[i] == '?') {
        snprintf(tmp, sizeof tmp, "%d", last_status);
        word_puts(w, tmp, sp);
        *pi = i + 1;
        return;
    }
    if (s[i] == '$') {
        snprintf(tmp, sizeof tmp, "%d", (int)getpid());
        word_puts(w, tmp, sp);
        *pi = i + 1;
        return;
    }
    if (s[i] == '!') {
        if (last_bg_pid) {
            snprintf(tmp, sizeof tmp, "%d", (int)last_bg_pid);
            word_puts(w, tmp, sp);
        }
        *pi = i + 1;
        return;
    }
    if (s[i] == '{') {
        if (expand_braced(s, pi, w, sp) == 0) return;
        word_putc(w, '$', 0); /* not a valid ${...}: keep '$' literally */
        *pi = i;
        return;
    }
    if (isalpha((unsigned char)s[i]) || s[i] == '_') {
        while ((isalnum((unsigned char)s[i]) || s[i] == '_') && k < sizeof name - 1)
            name[k++] = s[i++];
        name[k] = '\0';
        v = getenv(name);
        if (v) word_puts(w, v, sp);
        *pi = i;
        return;
    }
    word_putc(w, '$', 0);
    *pi = i;
}

/*
 * Remove quotes and expand $vars, $(cmd), `cmd` and ~.
 *   *glob   set when the result has an unquoted wildcard
 *   *quoted set when any quoting was present ("" stays an empty argument,
 *           an unset $VAR disappears)
 *   *maskp  (optional) per-char mask of splittable characters; caller frees
 */
static char *expand_word(const char *s, int *glob, int *quoted, char **maskp) {
    Word w;
    size_t i = 0;
    word_init(&w);
    *glob = 0;
    *quoted = 0;

    while (s[i]) {
        char c = s[i];
        if (c == '\\') {
            *quoted = 1;
            i++;
            if (s[i]) word_putc(&w, s[i++], 0);
        } else if (c == '\'') {
            *quoted = 1;
            i++;
            while (s[i] && s[i] != '\'') word_putc(&w, s[i++], 0);
            if (s[i]) i++;
        } else if (c == '"') {
            *quoted = 1;
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\' && s[i + 1] && strchr("\"\\$`", s[i + 1])) {
                    word_putc(&w, s[i + 1], 0);
                    i += 2;
                } else if (s[i] == '$') {
                    expand_var(s, &i, &w, 0);
                } else if (s[i] == '`') {
                    expand_backtick(s, &i, &w, 0);
                } else {
                    word_putc(&w, s[i++], 0);
                }
            }
            if (s[i]) i++;
        } else if (c == '$' || c == '`') {
            size_t before = w.b.len;
            if (c == '$') expand_var(s, &i, &w, 1);
            else expand_backtick(s, &i, &w, 1);
            for (size_t k = before; k < w.b.len; k++)
                if (strchr("*?[", w.b.s[k])) *glob = 1;
        } else if (c == '~' && i == 0 && (s[1] == '\0' || s[1] == '/')) {
            const char *h = getenv("HOME");
            if (h) word_puts(&w, h, 0);
            else word_putc(&w, '~', 0);
            i++;
        } else {
            if (c == '*' || c == '?' || c == '[') *glob = 1;
            word_putc(&w, c, 0);
            i++;
        }
    }
    if (maskp) *maskp = w.m.s;
    else free(w.m.s);
    return w.b.s;
}

/* ------------------------------------------------------------------ */
/* 4. parser                                                           */
/* ------------------------------------------------------------------ */

typedef enum { R_IN, R_OUT, R_APPEND, R_DUP, R_HEREDOC } RedirKind;

typedef struct {
    RedirKind kind;
    int fd;     /* fd being redirected */
    int dupfd;  /* R_DUP: fd to copy; R_HEREDOC: 1 if the body undergoes expansion */
    char *path; /* file name, or the here-document body */
} Redir;

typedef struct Compound Compound;

typedef struct {
    char **argv; /* raw words (expanded later, into an Exec) */
    int argc;
    Redir *redirs;
    int nredirs;
    Compound *comp; /* ( ) { } if while until for, or a function definition */
} Cmd;

typedef struct {
    Cmd *cmds;
    int ncmds;
    int background;
    int negate; /* leading '!' */
    char *text; /* short command text for the job table */
} Pipeline;

typedef enum { C_SEQ, C_AND, C_OR } Conn;

typedef struct {
    Pipeline pl;
    Conn conn; /* how this pipeline connects to the NEXT one */
} Item;

typedef struct {
    Item *v;
    int n, cap;
} List;

typedef enum { K_SUBSHELL, K_GROUP, K_IF, K_WHILE, K_FOR, K_FUNC } CompKind;

struct Compound {
    CompKind kind;
    List body;       /* subshell / group / loop body / 'then' branch */
    List cond;       /* if / while condition */
    List els;        /* 'else' branch; an elif chain nests another if in here */
    int has_else;
    int until;       /* K_WHILE: run until the condition succeeds */
    char *name;      /* K_FOR: variable; K_FUNC: function name */
    char **words;    /* K_FOR word list (raw) */
    int nwords;
    int has_in;      /* K_FOR: 0 means iterate over "$@" */
    Compound *fbody; /* K_FUNC: body; ownership moves to the function table */
};

static void free_list(List *L);
static void free_compound(Compound *k);

static void free_cmd(Cmd *c) {
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    free(c->argv);
    for (int i = 0; i < c->nredirs; i++) free(c->redirs[i].path);
    free(c->redirs);
    if (c->comp) free_compound(c->comp);
    memset(c, 0, sizeof *c);
}

static void free_pipeline(Pipeline *p) {
    for (int i = 0; i < p->ncmds; i++) free_cmd(&p->cmds[i]);
    free(p->cmds);
    free(p->text);
    memset(p, 0, sizeof *p);
}

static void free_list(List *L) {
    for (int i = 0; i < L->n; i++) free_pipeline(&L->v[i].pl);
    free(L->v);
    memset(L, 0, sizeof *L);
}

static void free_compound(Compound *k) {
    free_list(&k->body);
    free_list(&k->cond);
    free_list(&k->els);
    free(k->name);
    for (int i = 0; i < k->nwords; i++) free(k->words[i]);
    free(k->words);
    if (k->fbody) free_compound(k->fbody);
    free(k);
}

static Compound *new_compound(CompKind kind) {
    Compound *k = xmalloc(sizeof *k);
    memset(k, 0, sizeof *k);
    k->kind = kind;
    return k;
}

static void add_word(Cmd *c, const char *text) {
    c->argv = xrealloc(c->argv, (size_t)(c->argc + 2) * sizeof *c->argv);
    c->argv[c->argc++] = xstrdup(text);
    c->argv[c->argc] = NULL;
}

static void add_redir(Cmd *c, Redir r) {
    c->redirs = xrealloc(c->redirs, (size_t)(c->nredirs + 1) * sizeof *c->redirs);
    c->redirs[c->nredirs++] = r;
}

static void list_push(List *L, Pipeline pl, Conn conn) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 8;
        L->v = xrealloc(L->v, (size_t)L->cap * sizeof *L->v);
    }
    L->v[L->n].pl = pl;
    L->v[L->n].conn = conn;
    L->n++;
}

static const char *tok_name(TokType t) {
    switch (t) {
    case T_PIPE: return "|";
    case T_AMP: return "&";
    case T_SEMI: return ";";
    case T_AND: return "&&";
    case T_OR: return "||";
    case T_LT: return "<";
    case T_GT: return ">";
    case T_GTGT: return ">>";
    case T_DUP: return ">&";
    case T_NL: return "newline";
    case T_LPAREN: return "(";
    case T_RPAREN: return ")";
    case T_HEREDOC: return "<<";
    case T_AMPGT: return "&>";
    case T_AMPGTGT: return "&>>";
    case T_EOF: return "end of input";
    default: return "word";
    }
}

/* ---- recursive-descent parser ----
 *
 *   list     := (pipeline [';' | '&' | NL | '&&' | '||'])*      stops at a terminator
 *   pipeline := ['!'] command ('|' command)*
 *   command  := simple | '(' list ')' | '{' list '}' | if | while | until | for | funcdef
 *
 * Every parse function returns 0 = ok, -1 = syntax error (already reported),
 * 1 = ran out of input (the caller should read another line and retry).
 */
typedef struct {
    TokList *t;
    int i;
} Parser;

static Token *cur(Parser *p) { return &p->t->v[p->i]; }

static int is_kw(const Token *tk, const char *kw) {
    return tk->type == T_WORD && strcmp(tk->text, kw) == 0;
}

static int syntax(Parser *p) {
    Token *tk = cur(p);
    if (tk->type == T_EOF) return 1;
    warn_msg("syntax error near unexpected token `%s'",
             tk->type == T_WORD ? tk->text : tok_name(tk->type));
    return -1;
}

static int expect_kw(Parser *p, const char *kw) {
    if (is_kw(cur(p), kw)) {
        p->i++;
        return 0;
    }
    return syntax(p);
}

static void skip_nl(Parser *p) {
    while (cur(p)->type == T_NL) p->i++;
}

/* tokens that end a list: closing keywords, ')' and end of input */
static int at_terminator(Parser *p) {
    static const char *const kws[] = {"then", "elif", "else", "fi", "do", "done", "}", NULL};
    Token *tk = cur(p);
    if (tk->type == T_EOF || tk->type == T_RPAREN) return 1;
    if (tk->type != T_WORD) return 0;
    for (int i = 0; kws[i]; i++)
        if (is_kw(tk, kws[i])) return 1;
    return 0;
}

static int is_redir_tok(TokType ty) {
    return ty == T_LT || ty == T_GT || ty == T_GTGT || ty == T_DUP || ty == T_AMPGT ||
           ty == T_AMPGTGT || ty == T_HEREDOC;
}

static int parse_list(Parser *p, List *L);
static int parse_command(Parser *p, Cmd *c);

static int parse_redirs(Parser *p, Cmd *c) {
    while (is_redir_tok(cur(p)->type)) {
        Token *tk = cur(p);
        Redir r;
        int both = tk->type == T_AMPGT || tk->type == T_AMPGTGT;
        memset(&r, 0, sizeof r);
        r.fd = both ? 1 : tk->fd;
        p->i++;
        if (tk->type == T_DUP) {
            r.kind = R_DUP;
            r.dupfd = tk->dup;
        } else if (tk->type == T_HEREDOC) {
            r.kind = R_HEREDOC;
            r.path = xstrdup(tk->body ? tk->body : "");
            r.dupfd = (tk->dup & 2) ? 0 : 1; /* quoted delimiter: no expansion */
        } else {
            Token *tg = cur(p);
            if (tg->type != T_WORD) return syntax(p);
            r.kind = tk->type == T_LT ? R_IN
                     : (tk->type == T_GT || tk->type == T_AMPGT) ? R_OUT : R_APPEND;
            r.path = xstrdup(tg->text);
            p->i++;
        }
        add_redir(c, r);
        if (both) { /* &> file  ==  > file 2>&1 */
            Redir d;
            memset(&d, 0, sizeof d);
            d.kind = R_DUP;
            d.fd = 2;
            d.dupfd = 1;
            add_redir(c, d);
        }
    }
    return 0;
}

static int parse_simple(Parser *p, Cmd *c) {
    for (;;) {
        Token *tk = cur(p);
        if (tk->type == T_WORD) {
            add_word(c, tk->text);
            p->i++;
        } else if (is_redir_tok(tk->type)) {
            int rc = parse_redirs(p, c);
            if (rc) return rc;
        } else {
            break;
        }
    }
    if (c->argc == 0 && c->nredirs == 0) return syntax(p);
    return 0;
}

static int parse_subshell(Parser *p, Cmd *c) {
    Compound *k = new_compound(K_SUBSHELL);
    p->i++; /* '(' */
    int rc = parse_list(p, &k->body);
    if (!rc && cur(p)->type != T_RPAREN) rc = syntax(p);
    if (rc) {
        free_compound(k);
        return rc;
    }
    p->i++; /* ')' */
    c->comp = k;
    return 0;
}

static int parse_group(Parser *p, Cmd *c) {
    Compound *k = new_compound(K_GROUP);
    p->i++; /* '{' */
    int rc = parse_list(p, &k->body);
    if (!rc) rc = expect_kw(p, "}");
    if (rc) {
        free_compound(k);
        return rc;
    }
    c->comp = k;
    return 0;
}

static int parse_if(Parser *p, Cmd *c) {
    Compound *k = new_compound(K_IF);
    int rc, closed = 0;
    p->i++; /* 'if' or 'elif' */
    rc = parse_list(p, &k->cond);
    if (!rc) rc = expect_kw(p, "then");
    if (!rc) rc = parse_list(p, &k->body);
    if (!rc && is_kw(cur(p), "elif")) { /* elif c; then b ...  ==  else if c; then b ... fi */
        Cmd sub;
        memset(&sub, 0, sizeof sub);
        rc = parse_if(p, &sub); /* consumes the closing 'fi' */
        if (!rc) {
            Pipeline pl;
            memset(&pl, 0, sizeof pl);
            pl.cmds = xmalloc(sizeof *pl.cmds);
            pl.cmds[0] = sub;
            pl.ncmds = 1;
            pl.text = xstrdup("if ...");
            list_push(&k->els, pl, C_SEQ);
            k->has_else = 1;
            closed = 1;
        }
    } else if (!rc && is_kw(cur(p), "else")) {
        p->i++;
        rc = parse_list(p, &k->els);
        k->has_else = 1;
    }
    if (!rc && !closed) rc = expect_kw(p, "fi");
    if (rc) {
        free_compound(k);
        return rc;
    }
    c->comp = k;
    return 0;
}

static int parse_while(Parser *p, Cmd *c) {
    Compound *k = new_compound(K_WHILE);
    k->until = is_kw(cur(p), "until");
    p->i++;
    int rc = parse_list(p, &k->cond);
    if (!rc) rc = expect_kw(p, "do");
    if (!rc) rc = parse_list(p, &k->body);
    if (!rc) rc = expect_kw(p, "done");
    if (rc) {
        free_compound(k);
        return rc;
    }
    c->comp = k;
    return 0;
}

static int parse_for(Parser *p, Cmd *c) {
    Compound *k = new_compound(K_FOR);
    int rc;
    p->i++; /* 'for' */
    if (cur(p)->type != T_WORD) {
        rc = syntax(p);
        free_compound(k);
        return rc;
    }
    k->name = xstrdup(cur(p)->text);
    p->i++;
    skip_nl(p);
    if (is_kw(cur(p), "in")) {
        k->has_in = 1;
        p->i++;
        while (cur(p)->type == T_WORD) {
            k->words = xrealloc(k->words, (size_t)(k->nwords + 1) * sizeof *k->words);
            k->words[k->nwords++] = xstrdup(cur(p)->text);
            p->i++;
        }
    }
    while (cur(p)->type == T_SEMI || cur(p)->type == T_NL) p->i++;
    rc = expect_kw(p, "do");
    if (!rc) rc = parse_list(p, &k->body);
    if (!rc) rc = expect_kw(p, "done");
    if (rc) {
        free_compound(k);
        return rc;
    }
    c->comp = k;
    return 0;
}

/* name() compound   |   function name [()] compound */
static int parse_funcdef(Parser *p, Cmd *c, int has_kw) {
    Compound *k = new_compound(K_FUNC);
    Cmd body;
    int rc;
    if (has_kw) {
        p->i++; /* 'function' */
        if (cur(p)->type != T_WORD) {
            rc = syntax(p);
            free_compound(k);
            return rc;
        }
        k->name = xstrdup(cur(p)->text);
        p->i++;
        if (cur(p)->type == T_LPAREN) {
            p->i++;
            if (cur(p)->type != T_RPAREN) {
                rc = syntax(p);
                free_compound(k);
                return rc;
            }
            p->i++;
        }
    } else {
        k->name = xstrdup(cur(p)->text);
        p->i += 3; /* name ( ) */
    }
    skip_nl(p);
    rc = parse_command(p, &body);
    if (!rc && !body.comp) {
        warn_msg("syntax error: function body must be a compound command");
        free_cmd(&body);
        rc = -1;
    }
    if (rc) {
        free_compound(k);
        return rc;
    }
    k->fbody = body.comp;
    body.comp = NULL;
    free_cmd(&body);
    c->comp = k;
    return 0;
}

static int parse_command(Parser *p, Cmd *c) {
    Token *tk = cur(p);
    int rc;
    memset(c, 0, sizeof *c);
    if (tk->type == T_LPAREN) rc = parse_subshell(p, c);
    else if (is_kw(tk, "{")) rc = parse_group(p, c);
    else if (is_kw(tk, "if")) rc = parse_if(p, c);
    else if (is_kw(tk, "while") || is_kw(tk, "until")) rc = parse_while(p, c);
    else if (is_kw(tk, "for")) rc = parse_for(p, c);
    else if (is_kw(tk, "function")) rc = parse_funcdef(p, c, 1);
    else if (tk->type == T_WORD && p->t->v[p->i + 1].type == T_LPAREN &&
             p->t->v[p->i + 2].type == T_RPAREN)
        rc = parse_funcdef(p, c, 0);
    else
        return parse_simple(p, c);
    if (rc) return rc;
    rc = parse_redirs(p, c); /* redirections may follow a compound command */
    if (rc) free_cmd(c);
    return rc;
}

static void cmd_text(Buf *b, const Cmd *c) {
    if (c->comp) {
        switch (c->comp->kind) {
        case K_SUBSHELL: buf_puts(b, "( ... )"); break;
        case K_GROUP: buf_puts(b, "{ ... }"); break;
        case K_IF: buf_puts(b, "if ..."); break;
        case K_WHILE: buf_puts(b, c->comp->until ? "until ..." : "while ..."); break;
        case K_FOR:
            buf_puts(b, "for ");
            buf_puts(b, c->comp->name);
            buf_puts(b, " ...");
            break;
        case K_FUNC:
            buf_puts(b, c->comp->name);
            buf_puts(b, "()");
            break;
        }
        return;
    }
    for (int k = 0; k < c->argc; k++) {
        if (k) buf_putc(b, ' ');
        buf_puts(b, c->argv[k]);
    }
}

static int parse_pipeline(Parser *p, Pipeline *pl) {
    memset(pl, 0, sizeof *pl);
    if (is_kw(cur(p), "!")) {
        pl->negate = 1;
        p->i++;
    }
    for (;;) {
        Cmd c;
        int rc = parse_command(p, &c);
        if (rc) {
            free_pipeline(pl);
            return rc;
        }
        pl->cmds = xrealloc(pl->cmds, (size_t)(pl->ncmds + 1) * sizeof *pl->cmds);
        pl->cmds[pl->ncmds++] = c;
        if (cur(p)->type != T_PIPE) break;
        p->i++;
        skip_nl(p); /* a newline may follow '|' */
        if (cur(p)->type == T_EOF) {
            free_pipeline(pl);
            return 1;
        }
        if (pl->ncmds >= MAX_PROCS) {
            warn_msg("pipeline too long (max %d commands)", MAX_PROCS);
            free_pipeline(pl);
            return -1;
        }
    }
    Buf b;
    buf_init(&b);
    for (int i = 0; i < pl->ncmds; i++) {
        if (i) buf_puts(&b, " | ");
        cmd_text(&b, &pl->cmds[i]);
    }
    pl->text = b.s;
    return 0;
}

static int parse_list(Parser *p, List *L) {
    memset(L, 0, sizeof *L);
    for (;;) {
        skip_nl(p);
        if (at_terminator(p)) return 0;
        Pipeline pl;
        int rc = parse_pipeline(p, &pl);
        if (rc) {
            free_list(L);
            return rc;
        }
        Conn conn = C_SEQ;
        Token *tk = cur(p);
        if (tk->type == T_AMP) {
            pl.background = 1;
            p->i++;
        } else if (tk->type == T_SEMI || tk->type == T_NL) {
            p->i++;
        } else if (tk->type == T_AND || tk->type == T_OR) {
            conn = tk->type == T_AND ? C_AND : C_OR;
            p->i++;
            skip_nl(p); /* a newline may follow && and || */
            if (cur(p)->type == T_EOF) {
                free_pipeline(&pl);
                free_list(L);
                return 1;
            }
        } else if (!at_terminator(p)) {
            rc = syntax(p);
            free_pipeline(&pl);
            free_list(L);
            return rc;
        }
        list_push(L, pl, conn);
    }
}

static int parse(TokList *t, List *L) {
    Parser p = {t, 0};
    int rc = parse_list(&p, L);
    if (rc) return rc;
    if (cur(&p)->type != T_EOF) { /* stray ')' 'fi' 'done' ... */
        free_list(L);
        return syntax(&p);
    }
    return 0;
}

/* ---- expansion of parsed commands ----
 * The parsed tree is never modified: each execution expands the raw words
 * into a fresh Exec, so loop and function bodies can run any number of times. */

static void argv_push(char ***v, int *n, int *cap, char *s) {
    if (*n + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = xrealloc(*v, (size_t)*cap * sizeof **v);
    }
    (*v)[(*n)++] = s;
    (*v)[*n] = NULL;
}

/* Take ownership of w; glob-expand it if requested, then append to argv. */
static void push_field(char ***v, int *n, int *cap, char *w, int glob_flag) {
    if (glob_flag) {
        glob_t g;
        if (glob(w, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                argv_push(v, n, cap, xstrdup(g.gl_pathv[k]));
            globfree(&g);
            free(w);
            return;
        }
    }
    argv_push(v, n, cap, w);
}

/*
 * Expand raw words into a NULL-terminated argv: quotes, $vars, $(cmd), field
 * splitting of unquoted expansions, and globbing. Leading NAME=value words
 * (when allow_assign) are neither split nor globbed.
 */
static void expand_words(char **raw, int n, int allow_assign, char ***out, int *outc) {
    char **nv = NULL;
    int nc = 0, cap = 0, in_prefix = allow_assign;

    for (int i = 0; i < n; i++) {
        int glob_flag, quoted, split = 0;
        char *mask;
        int assign = in_prefix && is_assignment(raw[i]);
        if (!assign) in_prefix = 0;
        char *w = expand_word(raw[i], &glob_flag, &quoted, &mask);
        if (assign) glob_flag = 0;

        for (size_t k = 0; !assign && w[k]; k++) {
            if (mask[k] == '1' && isspace((unsigned char)w[k])) {
                split = 1;
                break;
            }
        }

        if (!split) {
            if (w[0] == '\0' && !quoted) free(w); /* unset variable: word vanishes */
            else push_field(&nv, &nc, &cap, w, glob_flag);
            free(mask);
            continue;
        }

        /* field splitting: break at whitespace that came from an unquoted expansion */
        Buf cur_field;
        buf_init(&cur_field);
        for (size_t k = 0; w[k]; k++) {
            if (mask[k] == '1' && isspace((unsigned char)w[k])) {
                if (cur_field.len) {
                    push_field(&nv, &nc, &cap, cur_field.s, glob_flag);
                    buf_init(&cur_field);
                }
            } else {
                buf_putc(&cur_field, w[k]);
            }
        }
        if (cur_field.len) push_field(&nv, &nc, &cap, cur_field.s, glob_flag);
        else free(cur_field.s);
        free(w);
        free(mask);
    }
    if (!nv) {
        nv = xmalloc(sizeof *nv);
        nv[0] = NULL;
    }
    *out = nv;
    *outc = nc;
}

/* Expand a here-document body: $var, $(cmd), `cmd`, and \$ \` \\ escapes. */
static char *expand_heredoc(const char *s) {
    Word w;
    size_t i = 0;
    word_init(&w);
    while (s[i]) {
        if (s[i] == '\\' && s[i + 1] && strchr("$`\\", s[i + 1])) {
            word_putc(&w, s[i + 1], 0);
            i += 2;
        } else if (s[i] == '$') {
            expand_var(s, &i, &w, 0);
        } else if (s[i] == '`') {
            expand_backtick(s, &i, &w, 0);
        } else {
            word_putc(&w, s[i++], 0);
        }
    }
    free(w.m.s);
    return w.b.s;
}

/* A command after expansion: what is actually about to run. */
typedef struct {
    char **argv;
    int argc;
    Redir *redirs;
    int nredirs;
} Exec;

static void expand_cmd(const Cmd *c, Exec *x) {
    expand_words(c->argv, c->argc, 1, &x->argv, &x->argc);
    x->nredirs = c->nredirs;
    x->redirs = c->nredirs ? xmalloc((size_t)c->nredirs * sizeof *x->redirs) : NULL;
    for (int i = 0; i < c->nredirs; i++) {
        const Redir *r = &c->redirs[i];
        x->redirs[i] = *r;
        if (r->kind == R_HEREDOC) {
            x->redirs[i].path = r->dupfd ? expand_heredoc(r->path) : xstrdup(r->path);
        } else if (r->path) {
            int g, q;
            x->redirs[i].path = expand_word(r->path, &g, &q, NULL);
        }
    }
}

static void free_exec(Exec *x) {
    for (int i = 0; i < x->argc; i++) free(x->argv[i]);
    free(x->argv);
    for (int i = 0; i < x->nredirs; i++) free(x->redirs[i].path);
    free(x->redirs);
    memset(x, 0, sizeof *x);
}

/* ------------------------------------------------------------------ */
/* 5. job control                                                      */
/* ------------------------------------------------------------------ */

typedef enum { J_UNUSED = 0, J_RUNNING, J_STOPPED, J_DONE } JobState;

typedef struct {
    int id;
    pid_t pgid;
    char *cmd;
    JobState state;
    pid_t pids[MAX_PROCS];
    int nproc;
    int remaining; /* processes not yet finished */
    pid_t last_pid;
    int status;  /* exit status of the last process in the pipeline */
    int termsig; /* signal that killed the last process, or 0 */
    int notify;  /* state change not yet reported to the user */
} Job;

static Job jobs[MAX_JOBS];
static int cur_job; /* id of the "current" job (marked +) */

static Job *alloc_job(const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_UNUSED) {
            memset(&jobs[i], 0, sizeof jobs[i]);
            jobs[i].id = i + 1;
            jobs[i].cmd = xstrdup(cmd);
            jobs[i].state = J_RUNNING;
            return &jobs[i];
        }
    }
    return NULL;
}

static void free_job(Job *j) {
    int id = j->id;
    free(j->cmd);
    memset(j, 0, sizeof *j);
    if (cur_job == id) {
        cur_job = 0;
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (jobs[i].state != J_UNUSED) {
                cur_job = jobs[i].id;
                break;
            }
        }
    }
}

static Job *find_job_by_pid(pid_t p) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_UNUSED) continue;
        for (int k = 0; k < jobs[i].nproc; k++)
            if (jobs[i].pids[k] == p) return &jobs[i];
    }
    return NULL;
}

static void print_job(FILE *f, const Job *j, const char *state) {
    fprintf(f, "[%d]%c  %-10s %s%s\n", j->id, j->id == cur_job ? '+' : ' ', state,
            j->cmd, j->state == J_RUNNING ? " &" : "");
}

/* Record a wait status for process p of job j. */
static void update_proc(Job *j, pid_t p, int st) {
    if (WIFSTOPPED(st)) {
        if (j->state != J_STOPPED) {
            j->state = J_STOPPED;
            j->notify = 1;
            cur_job = j->id;
        }
        return;
    }
    j->remaining--;
    if (p == j->last_pid) {
        if (WIFEXITED(st)) {
            j->status = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            j->termsig = WTERMSIG(st);
            j->status = 128 + j->termsig;
        }
    }
    if (j->remaining <= 0) j->state = J_DONE;
}

/* Send a signal to every process of a job. */
static int signal_job(const Job *j, int sig) {
    if (job_control) return kill(-j->pgid, sig);
    int rc = 0;
    for (int k = 0; k < j->nproc; k++)
        if (kill(j->pids[k], sig) < 0) rc = -1;
    return rc;
}

/* Block until job j finishes or stops (events of other jobs are recorded too). */
static void wait_foreground(Job *j) {
    int st;
    j->state = J_RUNNING;
    while (j->remaining > 0) {
        pid_t p = waitpid(-1, &st, WUNTRACED);
        if (p < 0) {
            if (errno == EINTR) continue;
            j->remaining = 0; /* ECHILD: nothing left to wait for */
            j->state = J_DONE;
            break;
        }
        Job *o = find_job_by_pid(p);
        if (o) update_proc(o, p, st);
        if (j->state == J_STOPPED) break;
    }
}

/*
 * Run job j in the foreground: give it the terminal, wait, take the terminal
 * back. If cont is set the job is sent SIGCONT first (fg on a stopped job).
 */
static int foreground_wait(Job *j, int cont) {
    int st;
    if (job_control) tcsetpgrp(shell_tty, j->pgid);
    if (cont) signal_job(j, SIGCONT);
    wait_foreground(j);
    if (job_control) {
        tcsetpgrp(shell_tty, shell_pgid);
        tcsetattr(shell_tty, TCSADRAIN, &shell_tmodes);
    }

    if (j->state == J_STOPPED) {
        if (interactive) {
            fputc('\n', stderr);
            print_job(stderr, j, "Stopped");
        }
        j->notify = 0;
        cur_job = j->id;
        st = 128 + SIGTSTP;
    } else {
        st = j->status;
        if (j->termsig == SIGINT) {
            if (interactive) fputc('\n', stderr);
        } else if (j->termsig && j->termsig != SIGPIPE) {
            fprintf(stderr, "%s\n", strsignal(j->termsig));
        }
        free_job(j);
    }
    last_status = st;
    return st;
}

/* Collect status changes of background children (non-blocking). */
static void reap_jobs(void) {
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
        Job *j = find_job_by_pid(p);
        if (j) update_proc(j, p, st);
    }
}

/* Tell the user about finished / stopped background jobs, free finished ones. */
static void notify_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (j->state == J_DONE) {
            if (interactive) {
                char st[64];
                if (j->termsig) snprintf(st, sizeof st, "%s", strsignal(j->termsig));
                else if (j->status) snprintf(st, sizeof st, "Exit %d", j->status);
                else snprintf(st, sizeof st, "Done");
                print_job(stderr, j, st);
            }
            free_job(j);
        } else if (j->state == J_STOPPED && j->notify) {
            if (interactive) print_job(stderr, j, "Stopped");
            j->notify = 0;
        }
    }
}

/* Parse a job spec: %n, n, %%, %+ or nothing (current job). */
static Job *find_job_arg(const char *arg, const char *who) {
    int id;
    if (!arg || strcmp(arg, "%%") == 0 || strcmp(arg, "%+") == 0) {
        id = cur_job;
        if (!id) {
            warn_msg("%s: no current job", who);
            return NULL;
        }
    } else {
        const char *p = arg + (arg[0] == '%');
        char *end;
        long v = strtol(p, &end, 10);
        if (*end || end == p || v < 1 || v > MAX_JOBS) {
            warn_msg("%s: %s: invalid job spec", who, arg);
            return NULL;
        }
        id = (int)v;
    }
    Job *j = &jobs[id - 1];
    if (j->state == J_UNUSED) {
        warn_msg("%s: %s: no such job", who, arg ? arg : "current");
        return NULL;
    }
    return j;
}

/* ------------------------------------------------------------------ */
/* 6. builtins                                                         */
/* ------------------------------------------------------------------ */

static void history_add(const char *line) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return;
    if (nhistory && strcmp(history[nhistory - 1], line) == 0) return;
    if (nhistory == MAX_HISTORY) {
        free(history[0]);
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof *history);
        nhistory--;
    }
    history[nhistory++] = xstrdup(line);
}

static int bi_cd(char **argv) {
    const char *target;
    int print = 0;
    if (argv[1] && argv[2]) {
        warn_msg("cd: too many arguments");
        return 1;
    }
    if (!argv[1]) {
        target = getenv("HOME");
        if (!target) {
            warn_msg("cd: HOME not set");
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        target = getenv("OLDPWD");
        if (!target) {
            warn_msg("cd: OLDPWD not set");
            return 1;
        }
        print = 1;
    } else {
        target = argv[1];
    }

    char old[PATH_MAX], cur[PATH_MAX];
    if (!getcwd(old, sizeof old)) old[0] = '\0';
    if (chdir(target) < 0) {
        fprintf(stderr, "heg: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (old[0]) setenv("OLDPWD", old, 1);
    if (getcwd(cur, sizeof cur)) {
        setenv("PWD", cur, 1);
        if (print) printf("%s\n", cur);
    }
    return 0;
}

static int bi_pwd(char **argv) {
    (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        perror("heg: pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int bi_exit(char **argv) {
    int code = last_status;
    if (argv[1]) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end || end == argv[1]) {
            warn_msg("exit: %s: numeric argument required", argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff);
        }
    }
    should_exit = 1;
    exit_code = code;
    return code;
}

static int bi_echo(char **argv) {
    int nl = 1, esc = 0, i = 1;
    for (; argv[i] && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p = argv[i] + 1;
        int ok = 1;
        for (; *p; p++) {
            if (!strchr("neE", *p)) {
                ok = 0;
                break;
            }
        }
        if (!ok) break;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'n') nl = 0;
            else if (*p == 'e') esc = 1;
            else esc = 0;
        }
    }
    for (int first = 1; argv[i]; i++, first = 0) {
        if (!first) putchar(' ');
        if (!esc) {
            fputs(argv[i], stdout);
            continue;
        }
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'a': putchar('\a'); break;
                case 'e': putchar(27); break;
                case '\\': putchar('\\'); break;
                case 'c': return 0; /* stop output, no newline */
                default:
                    putchar('\\');
                    putchar(*p);
                }
            } else {
                putchar(*p);
            }
        }
    }
    if (nl) putchar('\n');
    return 0;
}

static int valid_name(const char *s, size_t len) {
    if (len == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

static int bi_export(char **argv) {
    int rc = 0;
    if (!argv[1]) {
        for (char **e = environ; *e; e++) printf("export %s\n", *e);
        return 0;
    }
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nlen = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], nlen)) {
            warn_msg("export: `%s': not a valid identifier", argv[i]);
            rc = 1;
            continue;
        }
        char *name = xstrndup(argv[i], nlen);
        if (eq) setenv(name, eq + 1, 1);
        else if (!getenv(name)) setenv(name, "", 1);
        free(name);
    }
    return rc;
}

static int bi_unset(char **argv) {
    for (int i = 1; argv[i]; i++) unsetenv(argv[i]);
    return 0;
}

static int bi_jobs(char **argv) {
    (void)argv;
    for (int i = 0; i < MAX_JOBS; i++) {
        const Job *j = &jobs[i];
        if (j->state == J_UNUSED) continue;
        const char *st = j->state == J_RUNNING ? "Running"
                         : j->state == J_STOPPED ? "Stopped" : "Done";
        print_job(stdout, j, st);
    }
    return 0;
}

static int bi_fg(char **argv) {
    Job *j = find_job_arg(argv[1], "fg");
    if (!j) return 1;
    if (j->state == J_DONE) {
        warn_msg("fg: job %d has terminated", j->id);
        return 1;
    }
    printf("%s\n", j->cmd);
    fflush(stdout);
    j->notify = 0;
    return foreground_wait(j, 1);
}

static int bi_bg(char **argv) {
    Job *j = find_job_arg(argv[1], "bg");
    if (!j) return 1;
    if (j->state == J_DONE) {
        warn_msg("bg: job %d has terminated", j->id);
        return 1;
    }
    if (j->state == J_RUNNING) {
        warn_msg("bg: job %d already in background", j->id);
        return 0;
    }
    j->state = J_RUNNING;
    j->notify = 0;
    cur_job = j->id;
    signal_job(j, SIGCONT);
    printf("[%d]+ %s &\n", j->id, j->cmd);
    return 0;
}

static int bi_wait(char **argv) {
    (void)argv;
    int st = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (j->state != J_RUNNING) continue;
        wait_foreground(j);
        if (j->state == J_DONE) {
            st = j->status;
            free_job(j);
        }
    }
    return st;
}

static int parse_signal(const char *s) {
    static const struct {
        const char *name;
        int num;
    } tab[] = {{"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
               {"KILL", SIGKILL}, {"TERM", SIGTERM}, {"STOP", SIGSTOP},
               {"CONT", SIGCONT}, {"TSTP", SIGTSTP}, {"USR1", SIGUSR1},
               {"USR2", SIGUSR2}, {NULL, 0}};
    if (isdigit((unsigned char)s[0])) return atoi(s);
    if (strncmp(s, "SIG", 3) == 0) s += 3;
    for (int i = 0; tab[i].name; i++)
        if (strcmp(tab[i].name, s) == 0) return tab[i].num;
    return -1;
}

static int bi_kill(char **argv) {
    int sig = SIGTERM, i = 1, rc = 0;
    if (argv[1] && argv[1][0] == '-' && argv[1][1]) {
        sig = parse_signal(argv[1] + 1);
        if (sig < 0) {
            warn_msg("kill: %s: invalid signal specification", argv[1] + 1);
            return 1;
        }
        i = 2;
    }
    if (!argv[i]) {
        fprintf(stderr, "usage: kill [-SIGNAL] %%job|pid ...\n");
        return 1;
    }
    for (; argv[i]; i++) {
        if (argv[i][0] == '%') {
            Job *j = find_job_arg(argv[i], "kill");
            if (!j) {
                rc = 1;
                continue;
            }
            if (signal_job(j, sig) < 0) {
                fprintf(stderr, "heg: kill: %s\n", strerror(errno));
                rc = 1;
            } else if (j->state == J_STOPPED && sig != SIGSTOP && sig != SIGTSTP &&
                       sig != SIGCONT) {
                signal_job(j, SIGCONT); /* let a stopped job act on the signal */
            }
        } else {
            char *end;
            long p = strtol(argv[i], &end, 10);
            if (*end || end == argv[i]) {
                warn_msg("kill: %s: arguments must be job specs or process ids", argv[i]);
                rc = 1;
            } else if (kill((pid_t)p, sig) < 0) {
                fprintf(stderr, "heg: kill: (%ld) - %s\n", p, strerror(errno));
                rc = 1;
            }
        }
    }
    return rc;
}

static int bi_history(char **argv) {
    if (argv[1] && strcmp(argv[1], "-c") == 0) {
        for (int i = 0; i < nhistory; i++) free(history[i]);
        nhistory = 0;
        return 0;
    }
    for (int i = 0; i < nhistory; i++) printf("%5d  %s\n", i + 1, history[i]);
    return 0;
}

static int bi_source(char **argv) {
    if (!argv[1]) {
        warn_msg("source: filename argument required");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "heg: source: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    run_stream(f);
    fclose(f);
    return last_status;
}

/* ---- scripting builtins ---- */

static int bi_true(char **argv) {
    (void)argv;
    return 0;
}

static int bi_false(char **argv) {
    (void)argv;
    return 1;
}

/* break [n] / continue [n] / return [n] */
static int parse_count(const char *who, char **argv, int dflt) {
    if (!argv[1]) return dflt;
    char *end;
    long v = strtol(argv[1], &end, 10);
    if (*end || end == argv[1] || v < 0) {
        warn_msg("%s: %s: numeric argument required", who, argv[1]);
        return -1;
    }
    return (int)v;
}

static int bi_break(char **argv) {
    int n = parse_count("break", argv, 1);
    if (n < 1) return 1;
    ctl = CTL_BREAK;
    ctl_n = n;
    return 0;
}

static int bi_continue(char **argv) {
    int n = parse_count("continue", argv, 1);
    if (n < 1) return 1;
    ctl = CTL_CONTINUE;
    ctl_n = n;
    return 0;
}

static int bi_return(char **argv) {
    int n = parse_count("return", argv, last_status);
    if (n < 0) return 2;
    ctl = CTL_RETURN;
    return n & 0xff;
}

static int bi_shift(char **argv) {
    int n = parse_count("shift", argv, 1);
    if (n < 0 || n > npos) return 1;
    pos_args += n;
    npos -= n;
    return 0;
}

/* local NAME[=value]...: restored when the enclosing function returns */
typedef struct Local {
    char *name;
    char *old; /* previous value, or NULL if it was unset */
    struct Local *next;
} Local;

static Local *local_stack;

static void pop_local(void) {
    Local *l = local_stack;
    local_stack = l->next;
    if (l->old) setenv(l->name, l->old, 1);
    else unsetenv(l->name);
    free(l->name);
    free(l->old);
    free(l);
}

static int bi_local(char **argv) {
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nlen = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], nlen)) {
            warn_msg("local: `%s': not a valid identifier", argv[i]);
            return 1;
        }
        Local *l = xmalloc(sizeof *l);
        const char *old;
        l->name = xstrndup(argv[i], nlen);
        old = getenv(l->name);
        l->old = old ? xstrdup(old) : NULL;
        l->next = local_stack;
        local_stack = l;
        if (eq) setenv(l->name, eq + 1, 1);
        else unsetenv(l->name);
    }
    return 0;
}

/* read [-r] [name...]: one line from stdin, split into the names */
static int bi_read(char **argv) {
    int raw = 0, i = 1, eof = 0;
    char c;
    Buf b;
    if (argv[i] && strcmp(argv[i], "-r") == 0) {
        raw = 1;
        i++;
    }
    buf_init(&b);
    for (;;) { /* byte at a time: never consume more input than one line */
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            eof = 1;
            break;
        }
        if (c == '\n') break;
        if (!raw && c == '\\') {
            if (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') buf_putc(&b, c);
            continue;
        }
        buf_putc(&b, c);
    }
    const char *p = b.s;
    if (!argv[i]) {
        setenv("REPLY", p, 1);
    } else {
        for (; argv[i]; i++) {
            while (isspace((unsigned char)*p)) p++;
            if (argv[i + 1]) { /* not the last name: take one field */
                const char *e = p;
                while (*e && !isspace((unsigned char)*e)) e++;
                char *f = xstrndup(p, (size_t)(e - p));
                setenv(argv[i], f, 1);
                free(f);
                p = e;
            } else { /* last name gets the rest of the line */
                size_t len = strlen(p);
                while (len && isspace((unsigned char)p[len - 1])) len--;
                char *f = xstrndup(p, len);
                setenv(argv[i], f, 1);
                free(f);
            }
        }
    }
    free(b.s);
    return eof ? 1 : 0;
}

static int bi_eval(char **argv) {
    Buf b;
    buf_init(&b);
    for (int i = 1; argv[i]; i++) {
        if (i > 1) buf_putc(&b, ' ');
        buf_puts(&b, argv[i]);
    }
    last_status = 0;
    run_line(b.s);
    free(b.s);
    return last_status;
}

/* ---- test / [ ---- */

typedef struct {
    char **a;
    int i, n, err;
} TestP;

static int test_or(TestP *t);

static int is_binop(const char *s) {
    static const char *const ops[] = {"=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", NULL};
    for (int i = 0; ops[i]; i++)
        if (strcmp(ops[i], s) == 0) return 1;
    return 0;
}

static int test_unary(const char *op, const char *arg) {
    struct stat st;
    if (!strcmp(op, "-z")) return *arg == '\0';
    if (!strcmp(op, "-n")) return *arg != '\0';
    if (!strcmp(op, "-e")) return stat(arg, &st) == 0;
    if (!strcmp(op, "-f")) return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
    if (!strcmp(op, "-d")) return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
    if (!strcmp(op, "-s")) return stat(arg, &st) == 0 && st.st_size > 0;
    if (!strcmp(op, "-L") || !strcmp(op, "-h")) return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    if (!strcmp(op, "-r")) return access(arg, R_OK) == 0;
    if (!strcmp(op, "-w")) return access(arg, W_OK) == 0;
    if (!strcmp(op, "-x")) return access(arg, X_OK) == 0;
    return -1; /* not a unary operator */
}

static int test_primary(TestP *t) {
    if (t->i >= t->n) {
        t->err = 1;
        return 0;
    }
    const char *s = t->a[t->i];
    if (strcmp(s, "!") == 0) {
        t->i++;
        return !test_primary(t);
    }
    if (strcmp(s, "(") == 0) {
        t->i++;
        int v = test_or(t);
        if (t->i < t->n && strcmp(t->a[t->i], ")") == 0) t->i++;
        else t->err = 1;
        return v;
    }
    if (t->i + 2 < t->n && is_binop(t->a[t->i + 1])) {
        const char *op = t->a[t->i + 1], *b = t->a[t->i + 2];
        t->i += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(s, b) == 0;
        if (!strcmp(op, "!=")) return strcmp(s, b) != 0;
        char *e1, *e2;
        long x = strtol(s, &e1, 10), y = strtol(b, &e2, 10);
        if (*e1 || *e2 || e1 == s || e2 == b) {
            warn_msg("test: integer expression expected");
            t->err = 2;
            return 0;
        }
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y;
        return x >= y;
    }
    if (t->i + 1 < t->n && s[0] == '-' && s[1] && !s[2]) {
        int v = test_unary(s, t->a[t->i + 1]);
        if (v >= 0) {
            t->i += 2;
            return v;
        }
    }
    t->i++;
    return *s != '\0'; /* a lone string is true when non-empty */
}

static int test_and(TestP *t) {
    int v = test_primary(t);
    while (t->i < t->n && strcmp(t->a[t->i], "-a") == 0) {
        t->i++;
        int r = test_primary(t);
        v = v && r;
    }
    return v;
}

static int test_or(TestP *t) {
    int v = test_and(t);
    while (t->i < t->n && strcmp(t->a[t->i], "-o") == 0) {
        t->i++;
        int r = test_and(t);
        v = v || r;
    }
    return v;
}

static int bi_test(char **argv) {
    int argc = 0;
    while (argv[argc]) argc++;
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            warn_msg("[: missing `]'");
            return 2;
        }
        argc--;
    }
    if (argc <= 1) return 1; /* no expression: false */
    TestP t = {argv + 1, 0, argc - 1, 0};
    int v = test_or(&t);
    if (t.err == 2) return 2;
    if (t.err || t.i != t.n) {
        warn_msg("test: syntax error");
        return 2;
    }
    return v ? 0 : 1;
}

static int bi_help(char **argv) {
    (void)argv;
    puts("heg shell " HEG_VERSION "\n"
         "\n"
         "Builtins:\n"
         "  cd [dir|-]  pwd  echo [-neE]  export  unset  local  read [-r] [names]\n"
         "  true  false  :  test / [ ... ]  eval  type  history [-c]  source FILE  exit [n]\n"
         "  break [n]  continue [n]  return [n]  shift [n]\n"
         "  jobs  fg [%n]  bg [%n]  wait  kill [-SIG] %n|pid\n"
         "\n"
         "Syntax:\n"
         "  cmd | cmd    cmd > f  >> f  < f  2>&1  &> f  <<EOF ... EOF\n"
         "  a ; b   a && b   a || b   cmd &   ! cmd   ( subshell )   { group; }\n"
         "  if ...; then ...; elif ...; else ...; fi\n"
         "  while ...; do ...; done     until ...; do ...; done     for x in a b; do ...; done\n"
         "  name() { ...; }             $VAR ${VAR:-d} $(cmd) $((1+2)) $1 $# $@ $? $! ~ *.c");
    return 0;
}

typedef int (*builtin_fn)(char **);

static const struct {
    const char *name;
    builtin_fn fn;
} builtins[] = {
    {"cd", bi_cd},         {"pwd", bi_pwd},       {"exit", bi_exit},
    {"echo", bi_echo},     {"export", bi_export}, {"unset", bi_unset},
    {"jobs", bi_jobs},     {"fg", bi_fg},         {"bg", bi_bg},
    {"wait", bi_wait},     {"kill", bi_kill},     {"history", bi_history},
    {"source", bi_source}, {".", bi_source},      {"help", bi_help},
    {"true", bi_true},     {"false", bi_false},   {":", bi_true},
    {"test", bi_test},     {"[", bi_test},        {"break", bi_break},
    {"continue", bi_continue}, {"return", bi_return}, {"shift", bi_shift},
    {"local", bi_local},   {"read", bi_read},     {"eval", bi_eval},
    {"type", bi_type},     {NULL, NULL}};

static builtin_fn find_builtin(const char *name) {
    for (int i = 0; builtins[i].name; i++)
        if (strcmp(builtins[i].name, name) == 0) return builtins[i].fn;
    return NULL;
}

static int bi_type(char **argv) {
    int rc = 0;
    for (int i = 1; argv[i]; i++) {
        const char *n = argv[i];
        char buf[PATH_MAX];
        struct stat st;
        int found = 0;
        if (find_func(n)) {
            printf("%s is a function\n", n);
            continue;
        }
        if (find_builtin(n)) {
            printf("%s is a shell builtin\n", n);
            continue;
        }
        if (strchr(n, '/')) {
            if (stat(n, &st) == 0 && S_ISREG(st.st_mode) && access(n, X_OK) == 0) {
                printf("%s is %s\n", n, n);
                found = 1;
            }
        } else if (getenv("PATH")) {
            const char *p = getenv("PATH");
            while (*p && !found) {
                const char *e = strchr(p, ':');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                if (l == 0) snprintf(buf, sizeof buf, "./%s", n);
                else snprintf(buf, sizeof buf, "%.*s/%s", (int)l, p, n);
                if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0) {
                    printf("%s is %s\n", n, buf);
                    found = 1;
                }
                p = e ? e + 1 : p + l;
            }
        }
        if (!found) {
            fprintf(stderr, "heg: type: %s: not found\n", n);
            rc = 1;
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 7. executor                                                         */
/* ------------------------------------------------------------------ */

static int is_assignment(const char *s) {
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    while (isalnum((unsigned char)*s) || *s == '_') s++;
    return *s == '=';
}

/* open path and move it onto fd target */
static int redirect_file(const char *path, int flags, int target) {
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "heg: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fd != target) {
        if (dup2(fd, target) < 0) {
            perror("heg: dup2");
            close(fd);
            return -1;
        }
        close(fd);
    }
    return 0;
}

/* Apply redirections strictly left to right, so `2>&1 >f` != `>f 2>&1`. */
static int apply_redirs(const Exec *x) {
    for (int i = 0; i < x->nredirs; i++) {
        const Redir *r = &x->redirs[i];
        switch (r->kind) {
        case R_IN:
            if (redirect_file(r->path, O_RDONLY, r->fd) < 0) return -1;
            break;
        case R_OUT:
            if (redirect_file(r->path, O_WRONLY | O_CREAT | O_TRUNC, r->fd) < 0) return -1;
            break;
        case R_APPEND:
            if (redirect_file(r->path, O_WRONLY | O_CREAT | O_APPEND, r->fd) < 0) return -1;
            break;
        case R_DUP:
            if (r->dupfd != r->fd && dup2(r->dupfd, r->fd) < 0) {
                fprintf(stderr, "heg: %d: Bad file descriptor\n", r->dupfd);
                return -1;
            }
            break;
        case R_HEREDOC: {
            size_t len = strlen(r->path);
            int pf[2];
            if (len > 16384) {
                warn_msg("here-document too large (max 16 KiB)");
                return -1;
            }
            if (pipe(pf) < 0) {
                perror("heg: pipe");
                return -1;
            }
            if (len && write(pf[1], r->path, len) != (ssize_t)len) {
                perror("heg: write");
                close(pf[0]);
                close(pf[1]);
                return -1;
            }
            close(pf[1]);
            if (pf[0] != r->fd) {
                dup2(pf[0], r->fd);
                close(pf[0]);
            }
            break;
        }
        }
    }
    return 0;
}

/* ---- functions ---- */

typedef struct Func {
    char *name;
    Compound *body;
    struct Func *next;
} Func;

static Func *funcs;

static Func *find_func(const char *name) {
    for (Func *f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static void define_func(const char *name, Compound *body) {
    Func *f = find_func(name);
    if (f) {
        free_compound(f->body);
        f->body = body;
        return;
    }
    f = xmalloc(sizeof *f);
    f->name = xstrdup(name);
    f->body = body;
    f->next = funcs;
    funcs = f;
}

static int run_list(List *L);
static int run_compound(Compound *k);

static int call_function(Func *f, char **argv) {
    char **save = pos_args;
    int nsave = npos, n = 0;
    while (argv[n + 1]) n++;
    Local *mark = local_stack;
    pos_args = argv + 1;
    npos = n;
    int st = run_compound(f->body);
    if (ctl == CTL_RETURN) ctl = CTL_NONE;
    while (local_stack != mark) pop_local();
    pos_args = save;
    npos = nsave;
    return st;
}

/* ---- running a single command ---- */

static void set_assignment(const char *s) {
    const char *eq = strchr(s, '=');
    char *name = xstrndup(s, (size_t)(eq - s));
    setenv(name, eq + 1, 1);
    free(name);
}

static int all_assignments(const Exec *x) {
    for (int i = 0; i < x->argc; i++)
        if (!is_assignment(x->argv[i])) return 0;
    return x->argc > 0;
}

/* Does this command run inside the shell process (no fork needed)? */
static int is_inproc(const Cmd *c, const Exec *x) {
    if (c->comp) return c->comp->kind != K_SUBSHELL;
    if (x->argc == 0) return 1; /* redirections only */
    if (is_assignment(x->argv[0])) return 0; /* VAR=x cmd: needs a child */
    return find_func(x->argv[0]) != NULL || find_builtin(x->argv[0]) != NULL;
}

/* Run a compound command, function or builtin in the current process. */
static int run_body(const Cmd *c, const Exec *x) {
    if (c->comp) return run_compound(c->comp);
    if (x->argc == 0) return 0;
    Func *f = find_func(x->argv[0]);
    if (f) return call_function(f, x->argv);
    builtin_fn fn = find_builtin(x->argv[0]);
    return fn ? fn(x->argv) : 127;
}

/* run_body with redirections applied, then restored afterwards */
static int run_inproc(const Cmd *c, const Exec *x) {
    int saved[3], st;
    fflush(stdout);
    for (int i = 0; i < 3; i++) saved[i] = fcntl(i, F_DUPFD_CLOEXEC, 10);
    if (apply_redirs(x) < 0) st = 1;
    else st = run_body(c, x);
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        if (saved[i] >= 0) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
    return st;
}

static void run_child(const Cmd *c, const Exec *x, int idx, int n, int (*pipes)[2],
                      pid_t pgid, int fg) __attribute__((noreturn));

static void run_child(const Cmd *c, const Exec *x, int idx, int n, int (*pipes)[2],
                      pid_t pgid, int fg) {
    /* join the job's process group and (if foreground) take the terminal */
    if (job_control) {
        if (!pgid) pgid = getpid();
        setpgid(0, pgid);
        if (fg) tcsetpgrp(shell_tty, pgid);
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    /* background job in a script must not steal the script's stdin */
    if (!fg && !interactive && idx == 0) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd > 0) {
            dup2(fd, 0);
            close(fd);
        }
    }

    /* wire this command into the pipeline */
    if (idx > 0) dup2(pipes[idx - 1][0], STDIN_FILENO);
    if (idx < n - 1) dup2(pipes[idx][1], STDOUT_FILENO);
    for (int i = 0; i < n - 1; i++) { /* every pipe end must be closed, or readers hang */
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (apply_redirs(x) < 0) _exit(1);

    /* VAR=value cmd: the assignment applies to this child only */
    Exec y = *x;
    while (y.argc > 0 && is_assignment(y.argv[0])) {
        char *eq = strchr(y.argv[0], '=');
        *eq = '\0';
        setenv(y.argv[0], eq + 1, 1);
        y.argv++;
        y.argc--;
    }

    if (c->comp || y.argc == 0 || find_func(y.argv[0]) || find_builtin(y.argv[0])) {
        interactive = 0; /* this process is not a job-control shell */
        job_control = 0;
        int st = run_body(c, &y);
        fflush(NULL);
        _exit(st);
    }

    execvp(y.argv[0], y.argv);
    fprintf(stderr, "heg: %s: %s\n", y.argv[0],
            errno == ENOENT ? "command not found" : strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int run_pipeline_exec(Pipeline *pl, Exec *ex) {
    int n = pl->ncmds;

    /* commands that run inside the shell process itself */
    if (n == 1 && !pl->background) {
        const Cmd *c = &pl->cmds[0];
        const Exec *x = &ex[0];
        if (!c->comp && all_assignments(x)) {
            for (int i = 0; i < x->argc; i++) set_assignment(x->argv[i]);
            return last_status = 0;
        }
        if (is_inproc(c, x)) return last_status = run_inproc(c, x);
    }

    int (*pipes)[2] = n > 1 ? xmalloc((size_t)(n - 1) * sizeof *pipes) : NULL;
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("heg: pipe");
            for (int k = 0; k < i; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            free(pipes);
            return last_status = 1;
        }
    }

    Job *j = alloc_job(pl->text);
    if (!j) {
        warn_msg("too many jobs");
        for (int i = 0; i < n - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(pipes);
        return last_status = 1;
    }

    int fg = !pl->background;
    pid_t pgid = 0;
    fflush(NULL); /* don't let children inherit unflushed stdio buffers */

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("heg: fork");
            break;
        }
        if (pid == 0) run_child(&pl->cmds[i], &ex[i], i, n, pipes, pgid, fg);
        if (job_control) {
            if (!pgid) pgid = pid;
            setpgid(pid, pgid); /* also done in the child: avoids a race */
        }
        j->pids[j->nproc++] = pid;
    }

    for (int i = 0; i < n - 1; i++) { /* the shell keeps no pipe ends open */
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    free(pipes);

    if (j->nproc == 0) {
        free_job(j);
        return last_status = 1;
    }
    j->pgid = pgid;
    j->remaining = j->nproc;
    j->last_pid = j->pids[j->nproc - 1];

    if (fg) return foreground_wait(j, 0);

    j->state = J_RUNNING;
    cur_job = j->id;
    last_bg_pid = j->last_pid;
    if (interactive) fprintf(stderr, "[%d] %d\n", j->id, (int)j->last_pid);
    return last_status = 0;
}

static int run_pipeline(Pipeline *pl) {
    int n = pl->ncmds, st;
    Exec *ex = xmalloc((size_t)n * sizeof *ex);
    for (int i = 0; i < n; i++) expand_cmd(&pl->cmds[i], &ex[i]);
    st = run_pipeline_exec(pl, ex);
    for (int i = 0; i < n; i++) free_exec(&ex[i]);
    free(ex);
    if (pl->negate && !pl->background) st = last_status = !st;
    return st;
}

static int run_list(List *L) {
    for (int i = 0; i < L->n && !should_exit && ctl == CTL_NONE; i++) {
        if (i > 0) {
            Conn pc = L->v[i - 1].conn;
            if ((pc == C_AND && last_status != 0) || (pc == C_OR && last_status == 0))
                continue; /* skipped: status carries over to the next connector */
        }
        run_pipeline(&L->v[i].pl);
    }
    return last_status;
}

/*
 * Called after a loop body ran. Returns 1 if the loop must stop. break N and
 * continue N count down through ctl_n so they can reach an outer loop.
 */
static int loop_after_body(void) {
    if (ctl == CTL_NONE) return 0;
    if (ctl == CTL_RETURN) return 1;
    if (--ctl_n > 0) return 1; /* meant for an outer loop: leave ctl set */
    int brk = ctl == CTL_BREAK;
    ctl = CTL_NONE;
    return brk;
}

static int run_compound(Compound *k) {
    int st = 0;
    switch (k->kind) {
    case K_SUBSHELL: /* only reached inside the forked child that implements ( ) */
    case K_GROUP:
        return run_list(&k->body);

    case K_IF:
        run_list(&k->cond);
        if (ctl != CTL_NONE) return last_status;
        if (last_status == 0) run_list(&k->body);
        else if (k->has_else) run_list(&k->els);
        else last_status = 0;
        return last_status;

    case K_WHILE:
        for (;;) {
            run_list(&k->cond);
            if (ctl != CTL_NONE || should_exit) break;
            int ok = last_status == 0;
            if (k->until) ok = !ok;
            if (!ok) break;
            run_list(&k->body);
            st = last_status;
            if (should_exit || loop_after_body()) break;
        }
        return last_status = st;

    case K_FOR: {
        char **w;
        int n;
        if (k->has_in) {
            expand_words(k->words, k->nwords, 0, &w, &n);
        } else { /* for x; do ... : iterate over the positional parameters */
            w = xmalloc(((size_t)npos + 1) * sizeof *w);
            for (int i = 0; i < npos; i++) w[i] = xstrdup(pos_args[i]);
            w[npos] = NULL;
            n = npos;
        }
        for (int i = 0; i < n; i++) {
            setenv(k->name, w[i], 1);
            run_list(&k->body);
            st = last_status;
            if (should_exit || loop_after_body()) break;
        }
        for (int i = 0; i < n; i++) free(w[i]);
        free(w);
        return last_status = st;
    }

    case K_FUNC:
        if (k->fbody) { /* the function table takes ownership of the body */
            define_func(k->name, k->fbody);
            k->fbody = NULL;
        }
        return last_status = 0;
    }
    return 0;
}

/* Where input lines come from: a stream, a string (-c), or the terminal. */
typedef struct {
    FILE *f;
    const char *str;
    size_t pos;
    int tty;
} Source;

static char *tty_readline(int cont);

/* Next line without its newline (malloc'd), or NULL at end of input. */
static char *src_getline(Source *src, int cont) {
    if (src->tty) return tty_readline(cont);
    if (src->str) {
        if (!src->str[src->pos]) return NULL;
        size_t st = src->pos;
        while (src->str[src->pos] && src->str[src->pos] != '\n') src->pos++;
        char *l = xstrndup(src->str + st, src->pos - st);
        if (src->str[src->pos]) src->pos++;
        return l;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, src->f);
    if (n < 0) {
        free(line);
        return NULL;
    }
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    return line;
}

/* backslash-newline joins lines */
static char *strip_continuations(const char *s) {
    Buf b;
    buf_init(&b);
    for (; *s; s++) {
        if (*s == '\\' && s[1] == '\n') {
            s++;
            continue;
        }
        buf_putc(&b, *s);
    }
    return b.s;
}

/* Tokenize, parse and run text. Returns 1 if more input is needed first. */
static int run_text(const char *raw) {
    TokList t;
    List L;
    char *text = strip_continuations(raw);
    int rc = tokenize(text, &t);
    free(text);
    if (rc == TOK_INCOMPLETE) return 1;
    if (rc < 0) {
        last_status = 2;
        return 0;
    }
    if (t.n <= 1) { /* only EOF: blank line or comment */
        free_tokens(&t);
        return 0;
    }
    rc = parse(&t, &L);
    free_tokens(&t);
    if (rc == 1) return 1; /* the grammar needs more input (open if/while/quote...) */
    if (rc < 0) {
        last_status = 2;
        return 0;
    }
    run_list(&L);
    ctl = CTL_NONE;
    free_list(&L);
    return 0;
}

static void run_source(Source *src) {
    while (!should_exit) {
        char *line = src_getline(src, 0);
        if (!line) break;
        Buf acc;
        buf_init(&acc);
        buf_puts(&acc, line);
        free(line);
        while (run_text(acc.s) == 1) { /* incomplete: read a continuation line */
            char *more = src_getline(src, 1);
            if (!more) {
                if (line_cancelled) {
                    line_cancelled = 0;
                    last_status = 130;
                } else {
                    warn_msg("syntax error: unexpected end of file");
                    last_status = 2;
                }
                break;
            }
            buf_putc(&acc, '\n');
            buf_puts(&acc, more);
            free(more);
        }
        if (src->tty) history_add(acc.s);
        free(acc.s);
        reap_jobs();
        notify_jobs();
    }
}

static void run_line(const char *line) {
    Source s;
    memset(&s, 0, sizeof s);
    s.str = line;
    run_source(&s);
}

static void run_stream(FILE *f) {
    Source s;
    memset(&s, 0, sizeof s);
    s.f = f;
    run_source(&s);
}

/* ------------------------------------------------------------------ */
/* 8. interactive loop and main                                        */
/* ------------------------------------------------------------------ */

static char *history_path(void) {
    const char *h = getenv("HOME");
    if (!h) return NULL;
    size_t n = strlen(h) + 16;
    char *p = xmalloc(n);
    snprintf(p, n, "%s/.heg_history", h);
    return p;
}

static void history_load(void) {
    char *p = history_path();
    if (!p) return;
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n') line[n - 1] = '\0';
        for (char *q = line; *q; q++)
            if (*q == '\x1f') *q = '\n';
        history_add(line);
    }
    free(line);
    fclose(f);
}

static void history_save(void) {
    char *p = history_path();
    if (!p) return;
    FILE *f = fopen(p, "w");
    free(p);
    if (!f) return;
    for (int i = 0; i < nhistory; i++) {
        for (const char *q = history[i]; *q; q++) fputc(*q == '\n' ? '\x1f' : *q, f);
        fputc('\n', f);
    }
    fclose(f);
}

static void make_prompt(char *buf, size_t n) {
    char cwd[PATH_MAX], disp[PATH_MAX + 2];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == '\0' || cwd[hl] == '/'))
        snprintf(disp, sizeof disp, "~%s", cwd + hl);
    else
        snprintf(disp, sizeof disp, "%s", cwd);
    const char *col = last_status == 0 ? "32" : "31"; /* green / red */
    snprintf(buf, n, "\033[1;%sm" "heg" "\033[0m" ":" "\033[1;34m" "%s" "\033[0m" "$ ", col, disp);
}

/* ------------------------------------------------------------------ */
/* 8a. line editor: raw-mode input, history, cursor keys, completion   */
/* ------------------------------------------------------------------ */

enum { K_EOF = -1, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE };

typedef struct {
    char *s;
    size_t len, cap, pos; /* pos: byte offset of the cursor */
} Line;

static struct termios edit_saved;

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

/* Use the editor only on a capable terminal. */
static int editor_usable(void) {
    const char *t = getenv("TERM");
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && t && *t && strcmp(t, "dumb") != 0;
}

static int raw_on(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &edit_saved) < 0) return -1;
    t = edit_saved;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
}

static void raw_off(void) { tcsetattr(STDIN_FILENO, TCSADRAIN, &edit_saved); }

static int read_byte(unsigned char *c, int timeout_ms) {
    for (;;) {
        if (timeout_ms >= 0) {
            fd_set fds;
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return 0;
        }
        ssize_t n = read(STDIN_FILENO, c, 1);
        if (n < 0 && errno == EINTR) continue;
        return n == 1;
    }
}

static int read_key(void) {
    unsigned char c, a, b;
    if (!read_byte(&c, -1)) return K_EOF;
    if (c != 27) return c;
    if (!read_byte(&a, 50)) return 27;
    if (a == '[') {
        if (!read_byte(&b, 50)) return 27;
        switch (b) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        }
        if (isdigit(b)) {
            unsigned char t;
            if (!read_byte(&t, 50) || t != '~') return 27;
            if (b == '1' || b == '7') return K_HOME;
            if (b == '4' || b == '8') return K_END;
            if (b == '3') return K_DELETE;
        }
        return 27;
    }
    if (a == 'O') {
        if (!read_byte(&b, 50)) return 27;
        if (b == 'H') return K_HOME;
        if (b == 'F') return K_END;
        if (b == 'A') return K_UP;
        if (b == 'B') return K_DOWN;
        if (b == 'C') return K_RIGHT;
        if (b == 'D') return K_LEFT;
    }
    return 27;
}

/* ---- the line buffer (UTF-8 aware cursor movement) ---- */

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t prev_char(const Line *L, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && is_cont((unsigned char)L->s[pos])) pos--;
    return pos;
}

static size_t next_char(const Line *L, size_t pos) {
    if (pos >= L->len) return L->len;
    pos++;
    while (pos < L->len && is_cont((unsigned char)L->s[pos])) pos++;
    return pos;
}

static void line_set(Line *L, const char *s) {
    size_t n = strlen(s);
    if (n + 1 > L->cap) {
        L->cap = n + 64;
        L->s = xrealloc(L->s, L->cap);
    }
    memcpy(L->s, s, n + 1);
    L->len = L->pos = n;
}

static void line_insert(Line *L, const char *t, size_t n) {
    if (L->len + n + 1 > L->cap) {
        L->cap = (L->len + n + 1) * 2;
        L->s = xrealloc(L->s, L->cap);
    }
    memmove(L->s + L->pos + n, L->s + L->pos, L->len - L->pos + 1);
    memcpy(L->s + L->pos, t, n);
    L->len += n;
    L->pos += n;
}

static void line_delete(Line *L, size_t from, size_t to) { /* remove bytes [from, to) */
    if (to <= from) return;
    memmove(L->s + from, L->s + to, L->len - to + 1);
    L->len -= to - from;
    L->pos = from;
}

static int prompt_width(const char *p) {
    int w = 0;
    for (; *p; p++) {
        if (*p == '\033') { /* skip colour escapes */
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        if (!is_cont((unsigned char)*p)) w++;
    }
    return w;
}

/* Redraw the line, scrolling horizontally when it is wider than the terminal. */
static void refresh(const char *prompt, int pw, const Line *L, size_t *scroll) {
    int avail = term_cols() - pw - 1;
    if (avail < 8) avail = 8;
    size_t cpos = 0, clen = 0;
    for (size_t i = 0; i < L->len; i++) {
        if (is_cont((unsigned char)L->s[i])) continue;
        if (i < L->pos) cpos++;
        clen++;
    }
    if (cpos < *scroll) *scroll = cpos;
    if (cpos >= *scroll + (size_t)avail) *scroll = cpos - (size_t)avail + 1;

    Buf o;
    buf_init(&o);
    buf_putc(&o, '\r');
    buf_puts(&o, prompt);
    size_t ci = 0;
    for (size_t i = 0; i < L->len; i++) {
        unsigned char ch = (unsigned char)L->s[i];
        if (!is_cont(ch)) ci++;
        if (ci <= *scroll || ci > *scroll + (size_t)avail) continue;
        if (ch == '\n') buf_puts(&o, "\xE2\x86\xB5"); /* show a newline as a return arrow */
        else buf_putc(&o, (char)ch);
    }
    buf_puts(&o, "\033[K\r");
    int col = pw + (int)(cpos - *scroll);
    if (col > 0) {
        char t[32];
        snprintf(t, sizeof t, "\033[%dC", col);
        buf_puts(&o, t);
    }
    (void)clen;
    if (write(STDOUT_FILENO, o.s, o.len) < 0) { /* terminal gone: nothing to do */ }
    free(o.s);
}

/* ---- tab completion ---- */

typedef struct {
    char **v;
    int n, cap;
} StrList;

static void sl_add(StrList *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = xstrdup(s);
}

static void sl_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int is_exec_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static void complete_commands(StrList *out, const char *pre) {
    static const char *const kws[] = {"if", "then", "else", "elif", "fi", "while", "until",
                                      "do", "done", "for", "in", "function", NULL};
    size_t pl = strlen(pre);
    for (int i = 0; builtins[i].name; i++)
        if (strncmp(builtins[i].name, pre, pl) == 0) sl_add(out, builtins[i].name);
    for (int i = 0; kws[i]; i++)
        if (strncmp(kws[i], pre, pl) == 0) sl_add(out, kws[i]);
    for (Func *f = funcs; f; f = f->next)
        if (strncmp(f->name, pre, pl) == 0) sl_add(out, f->name);
    const char *path = getenv("PATH");
    while (path && *path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%.*s", (int)(l ? l : 1), l ? path : ".");
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                char full[PATH_MAX * 2];
                if (strncmp(de->d_name, pre, pl) != 0) continue;
                snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
                if (is_exec_file(full)) sl_add(out, de->d_name);
            }
            closedir(d);
        }
        path = e ? e + 1 : path + l;
    }
}

/* pre is what was typed (unescaped); candidates keep the typed directory part */
static void complete_files(StrList *out, const char *pre) {
    const char *slash = strrchr(pre, '/');
    size_t dl = slash ? (size_t)(slash - pre) + 1 : 0;
    const char *base = pre + dl;
    char *typed_dir = xstrndup(pre, dl);
    char dirpath[PATH_MAX];
    const char *home = getenv("HOME");
    if (typed_dir[0] == '~' && home) snprintf(dirpath, sizeof dirpath, "%s%s", home, typed_dir + 1);
    else snprintf(dirpath, sizeof dirpath, "%s", dl ? typed_dir : ".");
    DIR *d = opendir(dirpath);
    if (d) {
        struct dirent *de;
        size_t bl = strlen(base);
        while ((de = readdir(d)) != NULL) {
            char full[PATH_MAX * 2], cand[PATH_MAX * 2];
            struct stat st;
            if (strncmp(de->d_name, base, bl) != 0) continue;
            if (de->d_name[0] == '.' && base[0] != '.') continue; /* hidden: only if asked */
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(full, sizeof full, "%s/%s", dirpath, de->d_name);
            int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            snprintf(cand, sizeof cand, "%s%s%s", typed_dir, de->d_name, isdir ? "/" : "");
            sl_add(out, cand);
        }
        closedir(d);
    }
    free(typed_dir);
}

/* add backslashes before characters the shell would treat specially */
static void put_escaped(Buf *b, const char *s, int quoted) {
    for (; *s; s++) {
        if (!quoted && strchr(" \t\n\"'\\$&|;<>()*?[]#`", *s)) buf_putc(b, '\\');
        buf_putc(b, *s);
    }
}

static void print_candidates(const StrList *l) {
    int w = 0, cols = term_cols();
    for (int i = 0; i < l->n; i++) {
        const char *b = strrchr(l->v[i], '/');
        b = b && b[1] ? b + 1 : l->v[i];
        int bl = (int)strlen(b);
        if (bl > w) w = bl;
    }
    w += 2;
    int per = cols / w;
    if (per < 1) per = 1;
    (void)!write(STDOUT_FILENO, "\r\n", 2);
    for (int i = 0; i < l->n; i++) {
        const char *b = strrchr(l->v[i], '/');
        b = b && b[1] ? b + 1 : l->v[i];
        char t[PATH_MAX + 8];
        int n = snprintf(t, sizeof t, "%-*s", w, b);
        (void)!write(STDOUT_FILENO, t, (size_t)n);
        if ((i + 1) % per == 0 || i + 1 == l->n) (void)!write(STDOUT_FILENO, "\r\n", 2);
    }
}

/* Returns 1 if the line changed. *listed tells the caller candidates were printed. */
static int complete_word(Line *L, int *listed) {
    size_t s = L->pos;
    while (s > 0) {
        char p = L->s[s - 1];
        int escaped = s >= 2 && L->s[s - 2] == '\\';
        if (!escaped && (isspace((unsigned char)p) || strchr("|&;()<>", p))) break;
        s--;
    }
    /* what was typed, without backslashes or a leading quote */
    Buf typed;
    int quoted = 0;
    size_t i = s;
    buf_init(&typed);
    if (i < L->pos && (L->s[i] == '\'' || L->s[i] == '"')) {
        quoted = 1;
        i++;
    }
    for (; i < L->pos; i++) {
        if (L->s[i] == '\\' && i + 1 < L->pos) i++;
        buf_putc(&typed, L->s[i]);
    }

    /* command position: only blanks between the word and the start / an operator */
    size_t k = s;
    while (k > 0 && isspace((unsigned char)L->s[k - 1])) k--;
    int cmd_pos = k == 0 || strchr("|&;(", L->s[k - 1]) != NULL;

    StrList cands;
    memset(&cands, 0, sizeof cands);
    if (cmd_pos && !strchr(typed.s, '/')) complete_commands(&cands, typed.s);
    else complete_files(&cands, typed.s);
    qsort(cands.v, (size_t)cands.n, sizeof *cands.v, cmp_str);

    int changed = 0;
    if (cands.n > 0) {
        /* longest common prefix of all candidates */
        size_t lcp = strlen(cands.v[0]);
        for (int c = 1; c < cands.n; c++) {
            size_t m = 0;
            while (m < lcp && cands.v[c][m] == cands.v[0][m]) m++;
            lcp = m;
        }
        char *common = xstrndup(cands.v[0], lcp);
        Buf rep;
        buf_init(&rep);
        put_escaped(&rep, common, quoted);
        if (cands.n == 1) { /* unique: finish the word */
            size_t cl = strlen(common);
            if (cl == 0 || common[cl - 1] != '/') {
                if (quoted) buf_putc(&rep, L->s[s]);
                buf_putc(&rep, ' ');
            }
        }
        if (strlen(common) > strlen(typed.s) || cands.n == 1) {
            line_delete(L, s, L->pos);
            line_insert(L, rep.s, rep.len);
            changed = 1;
        } else if (cands.n > 1) {
            print_candidates(&cands);
            *listed = 1;
        }
        free(rep.s);
        free(common);
    }
    free(typed.s);
    sl_free(&cands);
    return changed;
}

/*
 * Read one line with editing. Returns a malloc'd string, or NULL on Ctrl-D at
 * an empty line. Ctrl-C returns "" (or NULL with line_cancelled set when cont).
 */
static char *editor_readline(const char *prompt, int cont) {
    Line L;
    size_t scroll = 0;
    int pw = prompt_width(prompt), hist = nhistory;
    char *saved = NULL;
    memset(&L, 0, sizeof L);
    line_set(&L, "");
    line_cancelled = 0;
    if (raw_on() < 0) return NULL;
    refresh(prompt, pw, &L, &scroll);

    for (;;) {
        int c = read_key(), listed = 0;
        if (c == K_EOF) {
            raw_off();
            free(L.s);
            free(saved);
            return NULL;
        }
        switch (c) {
        case '\r':
        case '\n': {
            L.pos = L.len;
            refresh(prompt, pw, &L, &scroll);
            (void)!write(STDOUT_FILENO, "\r\n", 2);
            raw_off();
            free(saved);
            return L.s;
        }
        case 3: /* Ctrl-C */
            (void)!write(STDOUT_FILENO, "^C\r\n", 4);
            raw_off();
            free(L.s);
            free(saved);
            if (cont) {
                line_cancelled = 1;
                return NULL;
            }
            return xstrdup("");
        case 4: /* Ctrl-D */
            if (L.len == 0) {
                raw_off();
                free(L.s);
                free(saved);
                return NULL;
            }
            line_delete(&L, L.pos, next_char(&L, L.pos));
            break;
        case 1: case K_HOME: L.pos = 0; break;
        case 5: case K_END: L.pos = L.len; break;
        case 2: case K_LEFT: L.pos = prev_char(&L, L.pos); break;
        case 6: case K_RIGHT: L.pos = next_char(&L, L.pos); break;
        case 127: case 8: line_delete(&L, prev_char(&L, L.pos), L.pos); break;
        case K_DELETE: line_delete(&L, L.pos, next_char(&L, L.pos)); break;
        case 21: { /* Ctrl-U: delete to start */
            size_t keep = L.pos;
            line_delete(&L, 0, keep);
            break;
        }
        case 11: /* Ctrl-K: delete to end */
            L.s[L.pos] = '\0';
            L.len = L.pos;
            break;
        case 23: { /* Ctrl-W: delete previous word */
            size_t p = L.pos;
            while (p > 0 && isspace((unsigned char)L.s[p - 1])) p--;
            while (p > 0 && !isspace((unsigned char)L.s[p - 1])) p--;
            line_delete(&L, p, L.pos);
            break;
        }
        case 12: /* Ctrl-L: clear screen */
            (void)!write(STDOUT_FILENO, "\033[H\033[2J", 7);
            break;
        case 16: case K_UP:
            if (hist > 0) {
                if (hist == nhistory) {
                    free(saved);
                    saved = xstrdup(L.s);
                }
                hist--;
                line_set(&L, history[hist]);
            }
            break;
        case 14: case K_DOWN:
            if (hist < nhistory) {
                hist++;
                line_set(&L, hist == nhistory ? (saved ? saved : "") : history[hist]);
            }
            break;
        case '\t':
            if (complete_word(&L, &listed) || listed) scroll = 0;
            break;
        default:
            if (c >= 32 && c < 256) {
                char ch = (char)c;
                line_insert(&L, &ch, 1);
            }
        }
        refresh(prompt, pw, &L, &scroll);
    }
}

/* Prompt and read one line from the terminal; NULL on Ctrl-D. */
static char *tty_readline(int cont) {
    char prompt[PATH_MAX + 128];
    if (!cont) {
        reap_jobs();
        notify_jobs();
        make_prompt(prompt, sizeof prompt);
    } else {
        snprintf(prompt, sizeof prompt, "> ");
    }
    if (editor_usable()) {
        char *l = editor_readline(prompt, cont);
        if (!l && !cont && !line_cancelled) fputs("exit\n", stderr);
        return l;
    }
    /* dumb terminal: plain cooked-mode input */
    static char *line;
    static size_t cap;
    fputs(prompt, stdout);
    fflush(stdout);
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        if (!cont) fputs("exit\n", stdout);
        return NULL;
    }
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    return xstrdup(line);
}

/* Become a proper job-control shell: own process group, own the terminal. */
static void init_interactive(void) {
    pid_t fgp;
    while ((fgp = tcgetpgrp(shell_tty)) != -1 && fgp != (shell_pgid = getpgrp()))
        kill(-shell_pgid, SIGTTIN);
    if (fgp == -1) { /* no controlling terminal: fall back to plain mode */
        interactive = 0;
        return;
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid); /* EPERM if we already lead a session: fine */
    tcsetpgrp(shell_tty, shell_pgid);
    tcgetattr(shell_tty, &shell_tmodes);
    job_control = 1;
}

static void cleanup(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_STOPPED) { /* don't leave stopped jobs behind */
            signal_job(&jobs[i], SIGHUP);
            signal_job(&jobs[i], SIGCONT);
        }
        if (jobs[i].state != J_UNUSED) free_job(&jobs[i]);
    }
    if (interactive) history_save();
    for (int i = 0; i < nhistory; i++) free(history[i]);
    nhistory = 0;
}

static void usage(FILE *f) {
    fprintf(f, "usage: heg [-c COMMAND [NAME [ARG...]] | SCRIPT [ARG...]]\n"
               "       heg --version\n");
}

int main(int argc, char **argv) {
    const char *cmd = NULL, *script = NULL;

    if (argc >= 2) {
        if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
            printf("heg shell %s\n", HEG_VERSION);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[1], "-c")) {
            if (argc < 3) {
                fprintf(stderr, "heg: -c: option requires an argument\n");
                return 2;
            }
            cmd = argv[2];
            if (argc > 3) shell_name = argv[3];
            if (argc > 4) {
                pos_args = argv + 4;
                npos = argc - 4;
            }
        } else {
            script = argv[1];
            shell_name = script;
            pos_args = argv + 2;
            npos = argc - 2;
        }
    }

    interactive = !cmd && !script && isatty(STDIN_FILENO);
    if (interactive) init_interactive();

    if (cmd) {
        run_line(cmd);
    } else if (script) {
        FILE *f = fopen(script, "r");
        if (!f) {
            fprintf(stderr, "heg: %s: %s\n", script, strerror(errno));
            return 127;
        }
        run_stream(f);
        fclose(f);
    } else if (interactive) {
        printf("heg shell %s -- type 'help' for builtins, Ctrl-D to exit\n", HEG_VERSION);
        history_load();
        {
            Source s;
            memset(&s, 0, sizeof s);
            s.tty = 1;
            run_source(&s);
        }
    } else {
        run_stream(stdin);
    }

    int rc = should_exit ? exit_code : last_status;
    cleanup();
    return rc;
}
