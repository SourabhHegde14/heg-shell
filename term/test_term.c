/* Headless tests for term.c, plus an end-to-end test that runs heg in a pty. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term.h"

static int passed, failed;

static void expect(const char *name, int ok) {
    if (ok) { passed++; printf("  ok    %s\n", name); }
    else { failed++; printf("  FAIL  %s\n", name); }
}

static char dumpbuf[8192];
static const char *dump(Term *t) { term_dump(t, dumpbuf, sizeof dumpbuf); return dumpbuf; }
static void feed(Term *t, const char *s) { term_feed(t, s, (int)strlen(s)); }
static int row_is(Term *t, int r, const char *want) {
    char b[256]; int n = 0;
    for (int c = 0; c < t->cols; c++) b[n++] = (char)t->grid[r * t->cols + c].ch;
    while (n && b[n - 1] == ' ') n--;
    b[n] = 0;
    return strcmp(b, want) == 0;
}

static void unit_tests(void) {
    Term *t = term_new(5, 10, 100);

    feed(t, "hello");
    expect("plain text", row_is(t, 0, "hello") && t->cx == 5);

    feed(t, "\r\nworld");
    expect("CR LF moves to next line start", row_is(t, 1, "world"));

    feed(t, "\033[2J\033[H");
    expect("clear screen + home", row_is(t, 0, "") && row_is(t, 1, "") && t->cx == 0 && t->cy == 0);

    feed(t, "abc\033[2Dx");
    expect("cursor left overwrites", row_is(t, 0, "axc"));

    feed(t, "\033[2;4Hz");
    expect("absolute positioning", t->grid[1 * 10 + 3].ch == 'z');

    feed(t, "\033[H\033[2Jabcdef\033[3D\033[K");
    expect("erase to end of line", row_is(t, 0, "abc"));

    feed(t, "\033[H\033[2Jabcdef\033[4G\033[P");
    expect("delete character", row_is(t, 0, "abcef"));

    feed(t, "\033[H\033[2Jabc\033[2G\033[@");
    expect("insert character", row_is(t, 0, "a bc"));

    feed(t, "\033[H\033[2J0123456789AB");
    expect("autowrap at the right edge", row_is(t, 0, "0123456789") && row_is(t, 1, "AB"));

    /* scrolling + scrollback */
    feed(t, "\033[H\033[2J");
    for (int i = 0; i < 8; i++) { char b[32]; snprintf(b, sizeof b, "line%d\r\n", i); feed(t, b); }
    expect("screen scrolls", row_is(t, 0, "line4") && t->sb_count == 4);
    const Cell *c = term_view_cell(t, 0, 0, 2); /* two lines into history */
    expect("scrollback is readable", c->ch == 'l' && term_view_cell(t, 0, 4, 2)->ch == '2');

    /* colours */
    feed(t, "\033[H\033[2J\033[1;32mG\033[0mN\033[38;5;196mR\033[38;2;1;2;3mT\033[41mB");
    const Cell *g = &t->grid[0], *nr = &t->grid[1], *r5 = &t->grid[2], *tc = &t->grid[3], *bgc = &t->grid[4];
    expect("SGR bold + green", g->fg == 2 && (g->attr & ATTR_BOLD));
    expect("SGR reset", nr->fg == COLOR_DEFAULT && nr->attr == 0);
    expect("256-colour", r5->fg == 196);
    expect("true colour", tc->fg == (0x1000000 | 0x010203));
    expect("background colour", bgc->bg == 1);

    /* reverse video */
    feed(t, "\033[H\033[2J\033[7mx");
    expect("SGR reverse", (t->grid[0].attr & ATTR_REVERSE) != 0);

    /* UTF-8 */
    feed(t, "\033[0m\033[H\033[2J\xc3\xa9\xe2\x82\xac");
    expect("UTF-8 decoding", t->grid[0].ch == 0xE9 && t->grid[1].ch == 0x20AC);

    /* sequences split across feeds */
    feed(t, "\033[H\033[2J"); feed(t, "\033["); feed(t, "3"); feed(t, "1mR");
    expect("escape split across reads", t->grid[0].ch == 'R' && t->grid[0].fg == 1);

    /* unknown sequences are swallowed */
    feed(t, "\033[H\033[2J\033[?9999h\033]0;title\007ok\033(B!");
    expect("unknown/OSC sequences ignored", row_is(t, 0, "ok!"));

    /* replies to the shell */
    feed(t, "\033[3;5H\033[6n");
    expect("cursor position report", t->reply_len > 0 && strcmp(t->reply, "\033[3;5R") == 0);
    t->reply_len = 0; memset(t->reply, 0, sizeof t->reply);

    /* alternate screen */
    feed(t, "\033[H\033[2Jmain\033[?1049halt");
    expect("alt screen starts blank, then shows alt content", t->grid[0].ch == ' ' && t->grid[4].ch == 'a');
    feed(t, "\033[H\033[2Jalt\033[?1049l");
    expect("alt screen restores main", row_is(t, 0, "main"));

    /* scroll region */
    feed(t, "\033[H\033[2J1\r\n2\r\n3\r\n4\r\n5\033[2;4r\033[4;1H\n");
    expect("scroll region scrolls only inside", row_is(t, 0, "1") && row_is(t, 1, "3") && row_is(t, 2, "4") && row_is(t, 4, "5"));
    feed(t, "\033[r");

    /* resize keeps content */
    feed(t, "\033[H\033[2Jkeep");
    term_resize(t, 8, 20);
    expect("resize keeps content", row_is(t, 0, "keep") && t->rows == 8 && t->cols == 20);
    term_resize(t, 2, 6);
    expect("shrink does not crash", t->rows == 2 && t->cols == 6);

    /* robustness: random bytes must never crash or break invariants */
    srand(1234);
    for (int i = 0; i < 20000; i++) {
        char b[64];
        int n = 1 + rand() % 63;
        for (int k = 0; k < n; k++) b[k] = (char)(rand() % 256);
        term_feed(t, b, n);
        if (i % 500 == 0) term_resize(t, 1 + rand() % 40, 1 + rand() % 100);
        if (t->cx < 0 || t->cx >= t->cols || t->cy < 0 || t->cy >= t->rows) { expect("cursor stays on screen under fuzz", 0); break; }
    }
    expect("20000 random-byte feeds, cursor always in bounds", t->cx >= 0 && t->cx < t->cols && t->cy >= 0 && t->cy < t->rows);
    term_free(t);
}

/* run heg inside a pty, feed its output to the screen model, check the screen */
static int pump(int fd, Term *t, int ms) {
    char b[4096]; int got = 0;
    struct pollfd p = {fd, POLLIN, 0};
    while (poll(&p, 1, ms) > 0) {
        ssize_t n = read(fd, b, sizeof b);
        if (n <= 0) break;
        term_feed(t, b, (int)n); got += (int)n;
        if (t->reply_len) { (void)!write(fd, t->reply, (size_t)t->reply_len); t->reply_len = 0; }
    }
    return got;
}

static void typeit(int fd, const char *s) { (void)!write(fd, s, strlen(s)); }

static void e2e_tests(const char *heg) {
    Term *t = term_new(24, 80, 1000);
    int fd;
    struct winsize ws = {24, 80, 0, 0};
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "", 1);
        execl(heg, heg, (char *)NULL);
        _exit(127);
    }
    pump(fd, t, 500);
    expect("heg banner appears on the emulated screen", strstr(dump(t), "heg shell") != NULL);

    typeit(fd, "echo hi | tr a-z A-Z\r");
    pump(fd, t, 600);
    expect("pipeline output rendered", strstr(dump(t), "\nHI\n") != NULL);

    typeit(fd, "echo abc\033[D\033[Dx\r");
    pump(fd, t, 600);
    expect("line editor works through the emulator (arrow keys)", strstr(dump(t), "\naxbc\n") != NULL);

    typeit(fd, "for i in 1 2 3; do echo n=$i; done\r");
    pump(fd, t, 600);
    expect("loop output rendered", strstr(dump(t), "n=1\nn=2\nn=3\n") != NULL);

    typeit(fd, "sleep 30\r");
    pump(fd, t, 300);
    typeit(fd, "\x03"); /* Ctrl-C */
    pump(fd, t, 400);
    typeit(fd, "echo alive\r");
    pump(fd, t, 500);
    expect("Ctrl-C kills the job, shell survives", strstr(dump(t), "\nalive\n") != NULL);

    typeit(fd, "\x04"); /* Ctrl-D */
    pump(fd, t, 500);
    int st = 0;
    pid_t w = 0;
    for (int i = 0; i < 20 && w == 0; i++) { w = waitpid(pid, &st, WNOHANG); if (!w) usleep(100000); }
    expect("Ctrl-D ends the shell", w == pid);
    if (w != pid) kill(pid, SIGKILL);
    close(fd);
    term_free(t);
}

int main(int argc, char **argv) {
    puts("== screen model ==");
    unit_tests();
    puts("== heg in a pty ==");
    e2e_tests(argc > 1 ? argv[1] : "./heg");
    printf("\npassed: %d   failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
