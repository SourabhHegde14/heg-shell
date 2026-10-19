/*
 * term.c -- grid + escape-sequence interpreter (a practical subset of xterm).
 *
 * Handled: printable UTF-8, \n \r \b \t \a, cursor movement (A B C D E F G H d f),
 * erase (J K X), insert/delete (L M P @), scrolling (S T, ESC M, scroll
 * regions), colours and attributes (SGR: 16, 256 and true colour), save/restore
 * cursor, alternate screen (?1049), cursor visibility (?25), device status
 * reports (ESC[6n) and device attributes (ESC[c).
 * Everything else is parsed and ignored, so unknown sequences never corrupt the screen.
 */
#include "term.h"

#include <stdlib.h>
#include <string.h>

enum { S_GROUND, S_ESC, S_ESC_SKIP, S_CSI, S_OSC, S_OSC_ESC };

static Cell blank_cell(const Term *t) {
    Cell c = {' ', COLOR_DEFAULT, t->cur_bg, 0};
    return c;
}

static Cell *at(Term *t, int r, int c) { return &t->grid[r * t->cols + c]; }

static void fill(Term *t, int r, int c0, int c1) { /* [c0, c1) on row r */
    Cell b = blank_cell(t);
    for (int c = c0; c < c1; c++) *at(t, r, c) = b;
}

static void touch(Term *t) { t->generation++; }

/* ---- scrollback ---- */

static void sb_push(Term *t, const Cell *line, int w) {
    if (t->sb_cap <= 0) return;
    int slot = t->sb_head;
    free(t->sb[slot]);
    t->sb[slot] = malloc((size_t)w * sizeof(Cell));
    if (!t->sb[slot]) { t->sb_w[slot] = 0; return; }
    memcpy(t->sb[slot], line, (size_t)w * sizeof(Cell));
    t->sb_w[slot] = w;
    t->sb_head = (slot + 1) % t->sb_cap;
    if (t->sb_count < t->sb_cap) t->sb_count++;
}

/* i = 0 is the newest line in history */
static const Cell *sb_line(const Term *t, int i, int *w) {
    int slot = ((t->sb_head - 1 - i) % t->sb_cap + t->sb_cap) % t->sb_cap;
    *w = t->sb_w[slot];
    return t->sb[slot];
}

/* ---- scrolling ---- */

static void scroll_up(Term *t, int n) { /* contents move up inside [top, bot] */
    int h = t->bot - t->top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    if (!t->alt_active && t->top == 0)
        for (int i = 0; i < n; i++) sb_push(t, at(t, i, 0), t->cols);
    memmove(at(t, t->top, 0), at(t, t->top + n, 0), (size_t)(h - n) * (size_t)t->cols * sizeof(Cell));
    for (int r = t->bot - n + 1; r <= t->bot; r++) fill(t, r, 0, t->cols);
}

static void scroll_down(Term *t, int n) {
    int h = t->bot - t->top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    memmove(at(t, t->top + n, 0), at(t, t->top, 0), (size_t)(h - n) * (size_t)t->cols * sizeof(Cell));
    for (int r = t->top; r < t->top + n; r++) fill(t, r, 0, t->cols);
}

static void index_down(Term *t) { /* line feed */
    if (t->cy == t->bot) scroll_up(t, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void index_up(Term *t) { /* reverse index */
    if (t->cy == t->top) scroll_down(t, 1);
    else if (t->cy > 0) t->cy--;
}

/* ---- construction ---- */

static Cell *new_grid(int rows, int cols) {
    Cell *g = malloc((size_t)rows * (size_t)cols * sizeof(Cell));
    if (!g) abort();
    Cell b = {' ', COLOR_DEFAULT, COLOR_DEFAULT, 0};
    for (int i = 0; i < rows * cols; i++) g[i] = b;
    return g;
}

Term *term_new(int rows, int cols, int scrollback_lines) {
    Term *t = calloc(1, sizeof *t);
    if (!t) abort();
    t->rows = rows; t->cols = cols;
    t->main_grid = new_grid(rows, cols);
    t->alt_grid = new_grid(rows, cols);
    t->grid = t->main_grid;
    t->sb_cap = scrollback_lines;
    if (scrollback_lines > 0) {
        t->sb = calloc((size_t)scrollback_lines, sizeof(Cell *));
        t->sb_w = calloc((size_t)scrollback_lines, sizeof(int));
    }
    t->top = 0; t->bot = rows - 1;
    t->cursor_visible = 1;
    t->cur_fg = t->cur_bg = COLOR_DEFAULT;
    return t;
}

void term_free(Term *t) {
    if (!t) return;
    for (int i = 0; i < t->sb_cap; i++) free(t->sb ? t->sb[i] : NULL);
    free(t->sb); free(t->sb_w);
    free(t->main_grid); free(t->alt_grid);
    free(t);
}

void term_resize(Term *t, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == t->rows && cols == t->cols) return;
    /* cursor below the new bottom: push the rows above it into history */
    if (!t->alt_active && t->cy >= rows) {
        int shift = t->cy - rows + 1;
        for (int i = 0; i < shift; i++) sb_push(t, at(t, i, 0), t->cols);
        memmove(t->main_grid, t->main_grid + (size_t)shift * (size_t)t->cols,
                (size_t)(t->rows - shift) * (size_t)t->cols * sizeof(Cell));
        t->cy -= shift;
    }
    Cell *grids[2] = {t->main_grid, t->alt_grid};
    for (int g = 0; g < 2; g++) {
        Cell *n = new_grid(rows, cols);
        int rr = rows < t->rows ? rows : t->rows, cc = cols < t->cols ? cols : t->cols;
        for (int r = 0; r < rr; r++)
            memcpy(n + (size_t)r * (size_t)cols, grids[g] + (size_t)r * (size_t)t->cols, (size_t)cc * sizeof(Cell));
        free(grids[g]);
        grids[g] = n;
    }
    t->main_grid = grids[0]; t->alt_grid = grids[1];
    t->grid = t->alt_active ? t->alt_grid : t->main_grid;
    t->rows = rows; t->cols = cols;
    t->top = 0; t->bot = rows - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cx >= cols) t->cx = cols - 1;
    t->saved_cx = t->saved_cy = 0;
    t->wrap_pending = 0;
    touch(t);
}

const Cell *term_view_cell(const Term *t, int r, int c, int offset) {
    static const Cell empty = {' ', COLOR_DEFAULT, COLOR_DEFAULT, 0};
    if (c < 0 || c >= t->cols) return &empty;
    int line = r - offset;
    if (line >= 0) return line < t->rows ? &t->grid[line * t->cols + c] : &empty;
    if (t->alt_active) return &empty;
    int back = -line - 1; /* 0 = newest history line */
    if (back >= t->sb_count) return &empty;
    int w;
    const Cell *l = sb_line(t, back, &w);
    return (l && c < w) ? &l[c] : &empty;
}

int term_dump(const Term *t, char *out, int outsz) {
    int n = 0;
    for (int r = 0; r < t->rows; r++) {
        int last = -1;
        for (int c = 0; c < t->cols; c++) if (t->grid[r * t->cols + c].ch != ' ') last = c;
        for (int c = 0; c <= last; c++) {
            uint32_t ch = t->grid[r * t->cols + c].ch;
            if (n + 5 >= outsz) { out[n] = 0; return n; }
            if (ch < 0x80) out[n++] = (char)ch;
            else if (ch < 0x800) { out[n++] = (char)(0xC0 | ch >> 6); out[n++] = (char)(0x80 | (ch & 63)); }
            else if (ch < 0x10000) { out[n++] = (char)(0xE0 | ch >> 12); out[n++] = (char)(0x80 | (ch >> 6 & 63)); out[n++] = (char)(0x80 | (ch & 63)); }
            else { out[n++] = (char)(0xF0 | ch >> 18); out[n++] = (char)(0x80 | (ch >> 12 & 63)); out[n++] = (char)(0x80 | (ch >> 6 & 63)); out[n++] = (char)(0x80 | (ch & 63)); }
        }
        if (n + 2 >= outsz) break;
        out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

/* ---- printing ---- */

static void put_char(Term *t, uint32_t ch) {
    if (t->wrap_pending) {
        t->cx = 0;
        index_down(t);
        t->wrap_pending = 0;
    }
    Cell *c = at(t, t->cy, t->cx);
    c->ch = ch; c->fg = t->cur_fg; c->bg = t->cur_bg; c->attr = t->cur_attr;
    if (t->cx == t->cols - 1) t->wrap_pending = 1;
    else t->cx++;
}

static void reply(Term *t, const char *s) {
    int n = (int)strlen(s);
    if (t->reply_len + n < (int)sizeof t->reply) {
        memcpy(t->reply + t->reply_len, s, (size_t)n);
        t->reply_len += n;
    }
}

/* ---- colours ---- */

static void sgr(Term *t) {
    if (t->nparams == 0) { t->params[0] = 0; t->nparams = 1; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) { t->cur_fg = t->cur_bg = COLOR_DEFAULT; t->cur_attr = 0; }
        else if (p == 1) t->cur_attr |= ATTR_BOLD;
        else if (p == 4) t->cur_attr |= ATTR_UNDERLINE;
        else if (p == 7) t->cur_attr |= ATTR_REVERSE;
        else if (p == 22) t->cur_attr &= (uint8_t)~ATTR_BOLD;
        else if (p == 24) t->cur_attr &= (uint8_t)~ATTR_UNDERLINE;
        else if (p == 27) t->cur_attr &= (uint8_t)~ATTR_REVERSE;
        else if (p >= 30 && p <= 37) t->cur_fg = p - 30;       /* palette index 0..7 */
        else if (p >= 90 && p <= 97) t->cur_fg = p - 90 + 8;
        else if (p >= 40 && p <= 47) t->cur_bg = p - 40;
        else if (p >= 100 && p <= 107) t->cur_bg = p - 100 + 8;
        else if (p == 39) t->cur_fg = COLOR_DEFAULT;
        else if (p == 49) t->cur_bg = COLOR_DEFAULT;
        else if (p == 38 || p == 48) {
            int *dst = p == 38 ? &t->cur_fg : &t->cur_bg;
            if (i + 2 < t->nparams && t->params[i + 1] == 5) {
                *dst = t->params[i + 2] & 255; /* palette index */
                i += 2;
            } else if (i + 4 < t->nparams && t->params[i + 1] == 2) {
                *dst = 0x1000000 | ((t->params[i + 2] & 255) << 16) | ((t->params[i + 3] & 255) << 8) | (t->params[i + 4] & 255);
                i += 4;
            } else {
                break;
            }
        }
    }
}
/* colour encoding: -1 default, 0..255 palette index, 0x1000000|rgb true colour */

/* ---- CSI ---- */

static int P(const Term *t, int i, int dflt) {
    return (i < t->nparams && t->params[i] > 0) ? t->params[i] : dflt;
}

static void clamp_cursor(Term *t) {
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void set_alt(Term *t, int on) {
    if (on == t->alt_active) return;
    if (on) {
        t->saved_cx = t->cx; t->saved_cy = t->cy;
        t->alt_active = 1;
        t->grid = t->alt_grid;
        for (int r = 0; r < t->rows; r++) fill(t, r, 0, t->cols);
    } else {
        t->alt_active = 0;
        t->grid = t->main_grid;
        t->cx = t->saved_cx; t->cy = t->saved_cy;
        clamp_cursor(t);
    }
    t->wrap_pending = 0;
}

static void csi(Term *t, char f) {
    int n;
    t->wrap_pending = (f == 'm' || f == 'h' || f == 'l') ? t->wrap_pending : 0;
    if (t->marker == '?') {
        if (f == 'h' || f == 'l') {
            int on = f == 'h';
            for (int i = 0; i < t->nparams; i++) {
                switch (t->params[i]) {
                case 1: t->app_cursor = on; break;
                case 25: t->cursor_visible = on; break;
                case 47: case 1047: case 1049: set_alt(t, on); break;
                case 2004: t->bracketed_paste = on; break;
                default: break;
                }
            }
        }
        return;
    }
    if (t->marker) { if (f == 'c') reply(t, "\033[>0;0;0c"); return; }
    switch (f) {
    case 'A': t->cy -= P(t, 0, 1); break;
    case 'B': t->cy += P(t, 0, 1); break;
    case 'C': t->cx += P(t, 0, 1); break;
    case 'D': t->cx -= P(t, 0, 1); break;
    case 'E': t->cy += P(t, 0, 1); t->cx = 0; break;
    case 'F': t->cy -= P(t, 0, 1); t->cx = 0; break;
    case 'G': case '`': t->cx = P(t, 0, 1) - 1; break;
    case 'd': t->cy = P(t, 0, 1) - 1; break;
    case 'H': case 'f': t->cy = P(t, 0, 1) - 1; t->cx = P(t, 1, 1) - 1; break;
    case 'J':
        n = t->nparams ? t->params[0] : 0;
        if (n == 0) { fill(t, t->cy, t->cx, t->cols); for (int r = t->cy + 1; r < t->rows; r++) fill(t, r, 0, t->cols); }
        else if (n == 1) { for (int r = 0; r < t->cy; r++) fill(t, r, 0, t->cols); fill(t, t->cy, 0, t->cx + 1); }
        else if (n == 2 || n == 3) { for (int r = 0; r < t->rows; r++) fill(t, r, 0, t->cols); }
        break;
    case 'K':
        n = t->nparams ? t->params[0] : 0;
        if (n == 0) fill(t, t->cy, t->cx, t->cols);
        else if (n == 1) fill(t, t->cy, 0, t->cx + 1);
        else if (n == 2) fill(t, t->cy, 0, t->cols);
        break;
    case 'X': n = P(t, 0, 1); fill(t, t->cy, t->cx, t->cx + n > t->cols ? t->cols : t->cx + n); break;
    case 'L': case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) {
            int saved_top = t->top;
            t->top = t->cy;
            if (f == 'L') scroll_down(t, P(t, 0, 1)); else scroll_up(t, P(t, 0, 1));
            t->top = saved_top;
        }
        break;
    case 'P': {
        n = P(t, 0, 1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(at(t, t->cy, t->cx), at(t, t->cy, t->cx + n), (size_t)(t->cols - t->cx - n) * sizeof(Cell));
        fill(t, t->cy, t->cols - n, t->cols);
        break;
    }
    case '@': {
        n = P(t, 0, 1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(at(t, t->cy, t->cx + n), at(t, t->cy, t->cx), (size_t)(t->cols - t->cx - n) * sizeof(Cell));
        fill(t, t->cy, t->cx, t->cx + n);
        break;
    }
    case 'S': scroll_up(t, P(t, 0, 1)); break;
    case 'T': scroll_down(t, P(t, 0, 1)); break;
    case 'r': {
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        if (top < 0) top = 0;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) { t->top = top; t->bot = bot; }
        t->cx = 0; t->cy = 0;
        break;
    }
    case 'm': sgr(t); break;
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;
    case 'n':
        if (P(t, 0, 0) == 6) {
            char b[32];
            int len = 0, v;
            b[len++] = '\033'; b[len++] = '[';
            char tmp[12]; int tl;
            v = t->cy + 1; tl = 0; do { tmp[tl++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (tl) b[len++] = tmp[--tl];
            b[len++] = ';';
            v = t->cx + 1; tl = 0; do { tmp[tl++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (tl) b[len++] = tmp[--tl];
            b[len++] = 'R'; b[len] = 0;
            reply(t, b);
        } else if (P(t, 0, 0) == 5) reply(t, "\033[0n");
        break;
    case 'c': reply(t, "\033[?1;2c"); break;
    default: break;
    }
    clamp_cursor(t);
}

/* ---- byte-at-a-time state machine ---- */

static void feed_byte(Term *t, unsigned char b) {
    switch (t->state) {
    case S_OSC:
        if (b == 7) t->state = S_GROUND;
        else if (b == 27) t->state = S_OSC_ESC;
        return;
    case S_OSC_ESC:
        t->state = S_GROUND; /* ESC \ ends it; anything else also abandons it */
        return;
    case S_ESC_SKIP: /* ESC ( B and friends: swallow the designator */
        t->state = S_GROUND;
        return;
    case S_ESC:
        t->state = S_GROUND;
        switch (b) {
        case '[': t->state = S_CSI; t->nparams = 0; t->marker = 0; memset(t->params, 0, sizeof t->params); t->cp = 0; return;
        case ']': t->state = S_OSC; return;
        case '(': case ')': case '*': case '+': case '#': t->state = S_ESC_SKIP; return;
        case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; return;
        case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; clamp_cursor(t); return;
        case 'M': index_up(t); return;
        case 'D': index_down(t); return;
        case 'E': t->cx = 0; index_down(t); return;
        case 'c': /* full reset */
            t->cur_fg = t->cur_bg = COLOR_DEFAULT; t->cur_attr = 0;
            t->top = 0; t->bot = t->rows - 1;
            set_alt(t, 0);
            for (int r = 0; r < t->rows; r++) fill(t, r, 0, t->cols);
            t->cx = t->cy = 0; t->cursor_visible = 1;
            return;
        default: return;
        }
    case S_CSI:
        if (b >= '0' && b <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 100000) *p = *p * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams < 16) t->nparams++;
        } else if (b == '?' || b == '>' || b == '=' || b == '<') {
            t->marker = b;
        } else if (b >= 0x40 && b <= 0x7e) {
            t->state = S_GROUND;
            csi(t, (char)b);
        } else if (b == 27) {
            t->state = S_ESC; /* sequence aborted by a new escape */
        } else if (b < 0x20) {
            /* control characters inside a CSI still act (rare) */
        }
        return;
    default:
        break;
    }

    /* ground state */
    if (t->utf_left > 0) {
        if ((b & 0xC0) == 0x80) {
            t->cp = (t->cp << 6) | (b & 0x3F);
            if (--t->utf_left == 0) put_char(t, t->cp);
            return;
        }
        t->utf_left = 0; /* malformed: fall through and treat b normally */
    }
    if (b >= 0x80) {
        if ((b & 0xE0) == 0xC0) { t->cp = b & 0x1F; t->utf_left = 1; }
        else if ((b & 0xF0) == 0xE0) { t->cp = b & 0x0F; t->utf_left = 2; }
        else if ((b & 0xF8) == 0xF0) { t->cp = b & 0x07; t->utf_left = 3; }
        else put_char(t, 0xFFFD);
        return;
    }
    switch (b) {
    case 27: t->state = S_ESC; break;
    case '\n': case 11: case 12: t->wrap_pending = 0; index_down(t); break;
    case '\r': t->cx = 0; t->wrap_pending = 0; break;
    case 8: if (t->cx > 0) t->cx--; t->wrap_pending = 0; break;
    case 9: {
        int nx = (t->cx / 8 + 1) * 8;
        t->cx = nx >= t->cols ? t->cols - 1 : nx;
        t->wrap_pending = 0;
        break;
    }
    case 7: break; /* bell */
    default:
        if (b >= 0x20 && b != 127) put_char(t, b);
        break;
    }
}

void term_feed(Term *t, const char *buf, int n) {
    for (int i = 0; i < n; i++) feed_byte(t, (unsigned char)buf[i]);
    touch(t);
}
