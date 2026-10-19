/*
 * term.h -- the screen model of hegterm: a character grid plus an ANSI/VT
 * escape-sequence parser. No SDL, no OS calls, so it can be tested headlessly.
 *
 *   bytes from the shell --term_feed()--> grid of cells --> a front-end draws it
 */
#ifndef HEGTERM_TERM_H
#define HEGTERM_TERM_H

#include <stdint.h>

#define COLOR_DEFAULT (-1)

enum { ATTR_BOLD = 1, ATTR_UNDERLINE = 2, ATTR_REVERSE = 4 };

typedef struct {
    uint32_t ch;  /* Unicode code point, ' ' when empty */
    int32_t fg;   /* 0xRRGGBB or COLOR_DEFAULT */
    int32_t bg;
    uint8_t attr;
} Cell;

typedef struct Term {
    int rows, cols;
    Cell *grid;                    /* the visible screen (main or alternate) */
    Cell *main_grid, *alt_grid;
    int alt_active;

    Cell **sb;                     /* scrollback ring of lines (main screen only) */
    int *sb_w;                     /* width of each stored line */
    int sb_cap, sb_count, sb_head; /* sb_head = slot that will be written next */

    int cx, cy, wrap_pending;
    int saved_cx, saved_cy;
    int top, bot;                  /* scroll region, inclusive */
    int cursor_visible;
    int app_cursor;                /* ESC[?1h : arrow keys send ESC O x */
    int bracketed_paste;           /* ESC[?2004h */

    int cur_fg, cur_bg;
    uint8_t cur_attr;

    int state, nparams, params[16], marker;
    uint32_t cp;
    int utf_left;

    char reply[64];                /* bytes the terminal must send BACK to the shell */
    int reply_len;
    unsigned long generation;      /* bumps on every change: tells the front-end to redraw */
} Term;

Term *term_new(int rows, int cols, int scrollback_lines);
void term_free(Term *t);
void term_feed(Term *t, const char *buf, int n);
void term_resize(Term *t, int rows, int cols);

/* Cell at screen row r, column c when scrolled `offset` lines into history. */
const Cell *term_view_cell(const Term *t, int r, int c, int offset);

/* The screen as text, one line per row, trailing blanks trimmed (for tests). */
int term_dump(const Term *t, char *out, int outsz);

#endif
