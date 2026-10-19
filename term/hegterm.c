/*
 * hegterm.c -- a Matrix-style terminal window for heg shell (SDL2 + SDL_ttf).
 *
 *   keyboard --SDL events--> bytes --> pty master ==== pty slave <-- heg
 *   screen   <-- SDL draw <-- term grid <--term_feed-- bytes <-- pty master
 *
 * The pty makes heg believe it runs in a real terminal, so job control and
 * the line editor work unchanged. All escape-sequence logic lives in term.c.
 *
 *   usage: hegterm [-s fontsize] [--true-colors] [--no-rain] [-- command args...]
 *   keys : F2 toggle digital rain   F3 toggle glow + scanlines
 *          Ctrl +/-  zoom           Shift+PgUp/PgDn / mouse wheel  scrollback
 *          Ctrl+Shift+V paste
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term.h"

#define PAD 12
#define SCROLLBACK 5000

/* ---- Matrix theme ---- */

#define NEON 0x00ff41u

static int theme_matrix = 1;
static uint32_t palette[256];

static void build_palette(void) {
    static const uint32_t base[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    for (int i = 0; i < 16; i++) palette[i] = base[i];
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++)
        palette[16 + i] = (uint32_t)(lv[i / 36] << 16 | lv[i / 6 % 6] << 8 | lv[i % 6]);
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        palette[232 + i] = (uint32_t)(v << 16 | v << 8 | v);
    }
}

/* every colour becomes a shade of green, keeping its brightness ordering */
static uint32_t matrixify(uint32_t rgb) {
    double r = (rgb >> 16 & 255) / 255.0, g = (rgb >> 8 & 255) / 255.0, b = (rgb & 255) / 255.0;
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    lum = 0.42 + 0.58 * lum; /* never darker than readable */
    int gg = (int)(255 * lum), bb = (int)(65 * lum);
    int tint = lum > 0.85 ? (int)((lum - 0.85) / 0.15 * 150) : 0; /* brightest -> glowing white-green */
    return (uint32_t)((tint << 16) | (gg << 8) | (bb > tint ? bb : tint));
}

static uint32_t color_rgb(int32_t c, int is_fg) {
    uint32_t rgb;
    if (c == COLOR_DEFAULT) return is_fg ? (theme_matrix ? NEON : 0xd0d0d0) : 0x000000;
    if (c & 0x1000000) rgb = (uint32_t)(c & 0xffffff);
    else rgb = palette[c & 255];
    return theme_matrix ? matrixify(rgb) : rgb;
}

/* ---- globals ---- */

static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *font;
static int font_size = 18, cw, chh;
static Term *term;
static int master = -1;
static pid_t child = -1;
static int scroll_off;
static int show_rain = 1, show_fx = 1;
static char font_path[512];
static Uint32 last_input;

/* ---- font + glyph cache ---- */

typedef struct { uint32_t key; SDL_Texture *tex; int w, h; } Glyph;
#define GCAP 8192
static Glyph gcache[GCAP];
static int gcount;

static void glyph_flush(void) {
    for (int i = 0; i < GCAP; i++) {
        if (gcache[i].tex) SDL_DestroyTexture(gcache[i].tex);
        gcache[i].tex = NULL; gcache[i].key = 0;
    }
    gcount = 0;
}

static Glyph *glyph(uint32_t cp, int bold) {
    uint32_t key = (cp << 1 | (uint32_t)bold) + 1;
    uint32_t h = (key * 2654435761u) % GCAP;
    for (;;) {
        Glyph *g = &gcache[h];
        if (g->key == key) return g->tex ? g : NULL;
        if (g->key == 0) {
            if (gcount > GCAP * 3 / 4) { glyph_flush(); return glyph(cp, bold); }
            SDL_Color white = {255, 255, 255, 255};
            if (!TTF_GlyphIsProvided32(font, cp)) cp = '?';
            TTF_SetFontStyle(font, bold ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
            SDL_Surface *s = TTF_RenderGlyph32_Blended(font, cp, white);
            g->key = key;
            gcount++;
            if (!s) return NULL;
            g->tex = SDL_CreateTextureFromSurface(ren, s);
            g->w = s->w; g->h = s->h;
            SDL_FreeSurface(s);
            SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
            return g->tex ? g : NULL;
        }
        h = (h + 1) % GCAP;
    }
}

static int load_font(int size) {
    const char *cand[] = {
        getenv("HEGTERM_FONT"),
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/Library/Fonts/Courier New.ttf",
        NULL};
    TTF_Font *f = NULL;
    if (font_path[0]) f = TTF_OpenFont(font_path, size);
    for (int i = 0; !f && i < 8; i++) {
        if (!cand[i]) continue;
        f = TTF_OpenFont(cand[i], size);
        if (f) snprintf(font_path, sizeof font_path, "%s", cand[i]);
    }
    if (!f) return -1;
    if (font) TTF_CloseFont(font);
    font = f;
    TTF_SizeUTF8(font, "M", &cw, NULL);
    chh = TTF_FontHeight(font);
    glyph_flush();
    return 0;
}

/* ---- pty ---- */

static void pty_send(const char *s, int n) {
    while (n > 0) {
        ssize_t w = write(master, s, (size_t)n);
        if (w > 0) { s += w; n -= (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {master, POLLOUT, 0};
            poll(&p, 1, 20);
            continue;
        }
        return;
    }
}

static void pty_resize(void) {
    struct winsize ws = {(unsigned short)term->rows, (unsigned short)term->cols, 0, 0};
    ioctl(master, TIOCSWINSZ, &ws);
}

static void spawn(char **cmd, int rows, int cols) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    child = forkpty(&master, NULL, NULL, &ws);
    if (child < 0) { perror("forkpty"); exit(1); }
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        if (cmd && cmd[0]) execvp(cmd[0], cmd);
        else {
            char exe[512], cand[600];
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (n > 0) {
                exe[n] = 0;
                char *slash = strrchr(exe, '/');
                if (slash) {
                    *slash = 0;
                    snprintf(cand, sizeof cand, "%s/heg", exe);
                    if (access(cand, X_OK) == 0) execl(cand, "heg", (char *)NULL);
                    snprintf(cand, sizeof cand, "%s/../heg", exe);
                    if (access(cand, X_OK) == 0) execl(cand, "heg", (char *)NULL);
                }
            }
            execlp("heg", "heg", (char *)NULL);
            const char *sh = getenv("SHELL");
            execl(sh ? sh : "/bin/sh", "sh", (char *)NULL);
        }
        perror("hegterm: cannot start shell");
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
}

/* returns 0 when the shell has exited */
static int pty_pump(void) {
    char buf[8192];
    for (;;) {
        ssize_t n = read(master, buf, sizeof buf);
        if (n > 0) {
            term_feed(term, buf, (int)n);
            if (term->reply_len) { pty_send(term->reply, term->reply_len); term->reply_len = 0; }
            continue;
        }
        if (n == 0 || (n < 0 && errno == EIO)) return 0;
        break; /* EAGAIN: nothing more right now */
    }
    int st;
    if (waitpid(child, &st, WNOHANG) == child) {
        /* drain whatever is left, then quit */
        for (;;) {
            ssize_t n = read(master, buf, sizeof buf);
            if (n <= 0) break;
            term_feed(term, buf, (int)n);
        }
        return 0;
    }
    return 1;
}

/* ---- digital rain ---- */

typedef struct { double head, speed; int len; uint32_t seed; } Drop;
static Drop *drops;
static int ndrops;
static uint32_t rng_state = 2463534242u;

static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static void drop_reset(Drop *d, int rows, int first) {
    d->len = 6 + (int)(rnd() % 18);
    d->speed = 4.0 + (double)(rnd() % 1000) / 1000.0 * 14.0;
    d->head = first ? -(double)(rnd() % (uint32_t)(rows + d->len)) : -(double)(rnd() % 20);
    d->seed = rnd();
}

static void rain_resize(void) {
    free(drops);
    ndrops = term->cols;
    drops = calloc((size_t)ndrops, sizeof *drops);
    for (int i = 0; i < ndrops; i++) drop_reset(&drops[i], term->rows, 1);
}

static void rain_step(double dt) {
    for (int i = 0; i < ndrops; i++) {
        drops[i].head += drops[i].speed * dt;
        if (drops[i].head - drops[i].len > term->rows) drop_reset(&drops[i], term->rows, 0);
    }
}

static const char RAIN_CHARS[] = "0123456789ABCDEFGHJKLMNPRSTUVXYZ<>=+-*/:|!?";

/* ---- drawing ---- */

static void set_col(SDL_Texture *t, uint32_t rgb, int alpha) {
    SDL_SetTextureColorMod(t, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
    SDL_SetTextureAlphaMod(t, (Uint8)alpha);
}

static void draw_glyph(uint32_t cp, int bold, int x, int y, uint32_t rgb, int alpha) {
    Glyph *g = glyph(cp, bold);
    if (!g) return;
    SDL_Rect d = {x + (cw - g->w) / 2, y, g->w, g->h};
    set_col(g->tex, rgb, alpha);
    SDL_RenderCopy(ren, g->tex, NULL, &d);
}

static void render(Uint32 now, double level) {
    int W, H;
    SDL_GetRendererOutputSize(ren, &W, &H);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    /* digital rain behind empty cells */
    if (show_rain && theme_matrix && level > 0.01) {
        /* keep text readable: no rain left of the last character on a row */
        int *rowlast = malloc((size_t)term->rows * sizeof(int));
        for (int r = 0; r < term->rows; r++) {
            int last = -1;
            for (int c = 0; c < term->cols; c++)
                if (term_view_cell(term, r, c, scroll_off)->ch != ' ') last = c;
            if (r == term->cy && scroll_off == 0 && term->cx > last) last = term->cx;
            rowlast[r] = last;
        }
        for (int c = 0; c < term->cols && c < ndrops; c++) {
            Drop *d = &drops[c];
            int r0 = (int)(d->head - d->len), r1 = (int)d->head;
            if (r0 < 0) r0 = 0;
            if (r1 >= term->rows) r1 = term->rows - 1;
            for (int r = r0; r <= r1; r++) {
                const Cell *cell = term_view_cell(term, r, c, scroll_off);
                if (cell->ch != ' ' || c <= rowlast[r] + 1) continue;
                double dist = d->head - r;
                if (dist < 0 || dist > d->len) continue;
                double b = 1.0 - dist / d->len;
                uint32_t h = (uint32_t)(c * 7919 + r * 104729) ^ d->seed;
                if (dist < 1.0) h += now / 70; /* the head flickers */
                char ch = RAIN_CHARS[h % (sizeof RAIN_CHARS - 1)];
                uint32_t rgb = dist < 1.0 ? 0xb4ffc8u : 0x00ff41u;
                draw_glyph((uint32_t)ch, 0, PAD + c * cw, PAD + r * chh, rgb, (int)(255 * b * b * level * 0.85));
            }
        }
        free(rowlast);
    }

    /* cell backgrounds, then glow, then text */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int r = 0; r < term->rows; r++) {
        for (int c = 0; c < term->cols; c++) {
            const Cell *cell = term_view_cell(term, r, c, scroll_off);
            uint32_t fg = color_rgb(cell->fg, 1), bg = color_rgb(cell->bg, 0);
            int has_bg = cell->bg != COLOR_DEFAULT;
            if (cell->attr & ATTR_REVERSE) {
                uint32_t t = fg;
                fg = cell->bg == COLOR_DEFAULT ? 0x000000 : bg;
                bg = cell->fg == COLOR_DEFAULT ? (theme_matrix ? NEON : 0xd0d0d0) : t;
                has_bg = 1;
            }
            int x = PAD + c * cw, y = PAD + r * chh;
            if (has_bg) {
                SDL_SetRenderDrawColor(ren, (Uint8)(bg >> 16), (Uint8)(bg >> 8), (Uint8)bg, 255);
                SDL_Rect rc = {x, y, cw, chh};
                SDL_RenderFillRect(ren, &rc);
            }
            if (cell->ch == ' ') continue;
            int bold = cell->attr & ATTR_BOLD;
            if (show_fx && theme_matrix && !has_bg) { /* soft halo */
                static const int off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int k = 0; k < 4; k++) draw_glyph(cell->ch, bold, x + off[k][0], y + off[k][1], fg, 55);
            }
            draw_glyph(cell->ch, bold, x, y, fg, 255);
            if (cell->attr & ATTR_UNDERLINE) {
                SDL_SetRenderDrawColor(ren, (Uint8)(fg >> 16), (Uint8)(fg >> 8), (Uint8)fg, 255);
                SDL_RenderDrawLine(ren, x, y + chh - 2, x + cw - 1, y + chh - 2);
            }
        }
    }

    /* cursor */
    if (term->cursor_visible && scroll_off == 0) {
        int focus = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        int solid = now - last_input < 600 || (now / 530) % 2 == 0;
        int x = PAD + term->cx * cw, y = PAD + term->cy * chh;
        SDL_Rect rc = {x, y, cw, chh};
        uint32_t cur = theme_matrix ? NEON : 0xd0d0d0;
        SDL_SetRenderDrawColor(ren, (Uint8)(cur >> 16), (Uint8)(cur >> 8), (Uint8)cur, 255);
        if (!focus) SDL_RenderDrawRect(ren, &rc);
        else if (solid) {
            SDL_RenderFillRect(ren, &rc);
            const Cell *cell = term_view_cell(term, term->cy, term->cx, 0);
            if (cell->ch != ' ') draw_glyph(cell->ch, cell->attr & ATTR_BOLD, x, y, 0x000000, 255);
        }
    }

    /* CRT feel: scanlines and a faint frame */
    if (show_fx && theme_matrix) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 60);
        for (int y = 0; y < H; y += 3) {
            SDL_Rect l = {0, y, W, 1};
            SDL_RenderFillRect(ren, &l);
        }
        SDL_SetRenderDrawColor(ren, 0, 150, 40, 255);
        SDL_Rect fr = {0, 0, W, H};
        SDL_RenderDrawRect(ren, &fr);
        SDL_SetRenderDrawColor(ren, 0, 70, 20, 255);
        SDL_Rect fr2 = {1, 1, W - 2, H - 2};
        SDL_RenderDrawRect(ren, &fr2);
    }
    SDL_RenderPresent(ren);
}

/* ---- window geometry ---- */

static void fit_to_window(void) {
    int W, H;
    SDL_GetRendererOutputSize(ren, &W, &H);
    int cols = (W - 2 * PAD) / cw, rows = (H - 2 * PAD) / chh;
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols != term->cols || rows != term->rows) {
        term_resize(term, rows, cols);
        pty_resize();
        rain_resize();
        scroll_off = 0;
    }
}

/* ---- keyboard ---- */

static void key_down(SDL_KeyboardEvent *k) {
    SDL_Keymod m = (SDL_Keymod)k->keysym.mod;
    int ctrl = (m & KMOD_CTRL) != 0, shift = (m & KMOD_SHIFT) != 0;
    SDL_Keycode s = k->keysym.sym;
    const char *seq = NULL;
    char one[2] = {0, 0};
    char esc[8];

    last_input = SDL_GetTicks();
    if (s == SDLK_F2) { show_rain = !show_rain; return; }
    if (s == SDLK_F3) { show_fx = !show_fx; return; }
    if (ctrl && (s == SDLK_EQUALS || s == SDLK_PLUS || s == SDLK_KP_PLUS || s == SDLK_MINUS || s == SDLK_KP_MINUS)) {
        int ns = font_size + ((s == SDLK_MINUS || s == SDLK_KP_MINUS) ? -2 : 2);
        if (ns >= 8 && ns <= 48 && load_font(ns) == 0) { font_size = ns; fit_to_window(); }
        return;
    }
    if (ctrl && shift && s == SDLK_v) {
        char *txt = SDL_GetClipboardText();
        if (txt) {
            if (term->bracketed_paste) pty_send("\033[200~", 6);
            pty_send(txt, (int)strlen(txt));
            if (term->bracketed_paste) pty_send("\033[201~", 6);
            SDL_free(txt);
        }
        scroll_off = 0;
        return;
    }
    if (shift && (s == SDLK_PAGEUP || s == SDLK_PAGEDOWN)) {
        int step = term->rows - 1;
        scroll_off += s == SDLK_PAGEUP ? step : -step;
        if (scroll_off < 0) scroll_off = 0;
        if (scroll_off > term->sb_count) scroll_off = term->sb_count;
        return;
    }
    switch (s) {
    case SDLK_RETURN: case SDLK_KP_ENTER: seq = "\r"; break;
    case SDLK_BACKSPACE: seq = "\x7f"; break;
    case SDLK_TAB: seq = "\t"; break;
    case SDLK_ESCAPE: seq = "\033"; break;
    case SDLK_UP: case SDLK_DOWN: case SDLK_RIGHT: case SDLK_LEFT: {
        char d = s == SDLK_UP ? 'A' : s == SDLK_DOWN ? 'B' : s == SDLK_RIGHT ? 'C' : 'D';
        snprintf(esc, sizeof esc, term->app_cursor ? "\033O%c" : "\033[%c", d);
        seq = esc;
        break;
    }
    case SDLK_HOME: seq = "\033[H"; break;
    case SDLK_END: seq = "\033[F"; break;
    case SDLK_DELETE: seq = "\033[3~"; break;
    case SDLK_INSERT: seq = "\033[2~"; break;
    case SDLK_PAGEUP: seq = "\033[5~"; break;
    case SDLK_PAGEDOWN: seq = "\033[6~"; break;
    default:
        if (ctrl && !shift) {
            if (s >= SDLK_a && s <= SDLK_z) { one[0] = (char)(s - SDLK_a + 1); seq = one; }
            else if (s == SDLK_SPACE) { one[0] = 0; pty_send(one, 1); scroll_off = 0; return; }
            else if (s == SDLK_LEFTBRACKET) { one[0] = 27; seq = one; }
            else if (s == SDLK_BACKSLASH) { one[0] = 28; seq = one; }
            else if (s == SDLK_RIGHTBRACKET) { one[0] = 29; seq = one; }
        }
        break;
    }
    if (seq) { pty_send(seq, (int)strlen(seq)); scroll_off = 0; }
}

/* ---- main ---- */

static void unescape(char *s) { /* \r \n \t \e \\ for --send */
    char *w = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *w++ = *s == 'r' ? '\r' : *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'e' ? 27 : *s;
        } else *w++ = *s;
    }
    *w = 0;
}

int main(int argc, char **argv) {
    const char *shot = NULL;
    char *send_text = NULL;
    int shot_after = 1500, send_after = 600;
    char **cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { cmd = &argv[i + 1]; break; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) font_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--true-colors")) theme_matrix = 0;
        else if (!strcmp(argv[i], "--no-rain")) show_rain = 0;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--shot-after") && i + 1 < argc) shot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) { send_text = argv[++i]; unescape(send_text); }
        else if (!strcmp(argv[i], "--send-after") && i + 1 < argc) send_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: hegterm [-s fontsize] [--true-colors] [--no-rain] [-- command args...]\n"
                 "keys:  F2 rain   F3 glow+scanlines   Ctrl +/- zoom   Shift+PgUp/PgDn scrollback   Ctrl+Shift+V paste");
            return 0;
        } else { fprintf(stderr, "hegterm: unknown option %s (try --help)\n", argv[i]); return 2; }
    }
    if (font_size < 8) font_size = 8;
    if (font_size > 48) font_size = 48;
    build_palette();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF: %s\n", TTF_GetError()); return 1; }
    if (load_font(font_size) != 0) {
        fprintf(stderr, "hegterm: no monospace font found; set HEGTERM_FONT=/path/to/font.ttf\n");
        return 1;
    }
    win = SDL_CreateWindow("heg shell", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           100 * cw + 2 * PAD, 30 * chh + 2 * PAD, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 1; }
    load_font(font_size); /* re-create glyph cache against this renderer */
    SDL_StartTextInput();
    signal(SIGPIPE, SIG_IGN);

    int W, H;
    SDL_GetRendererOutputSize(ren, &W, &H);
    int cols = (W - 2 * PAD) / cw, rows = (H - 2 * PAD) / chh;
    term = term_new(rows, cols, SCROLLBACK);
    rain_resize();
    spawn(cmd, rows, cols);

    Uint32 start = SDL_GetTicks(), prev = start;
    int running = 1, sent = 0;
    while (running) {
        Uint32 now = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN: key_down(&e.key); break;
            case SDL_TEXTINPUT:
                last_input = now;
                pty_send(e.text.text, (int)strlen(e.text.text));
                scroll_off = 0;
                break;
            case SDL_MOUSEWHEEL:
                scroll_off += e.wheel.y * 3;
                if (scroll_off < 0) scroll_off = 0;
                if (scroll_off > term->sb_count) scroll_off = term->sb_count;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)
                    fit_to_window();
                break;
            default: break;
            }
        }
        if (!pty_pump()) running = 0;
        if (send_text && !sent && now - start > (Uint32)send_after) { pty_send(send_text, (int)strlen(send_text)); sent = 1; }

        double dt = (now - prev) / 1000.0;
        prev = now;
        rain_step(dt);
        double t = (now - start) / 1000.0;
        double level = t < 1.0 ? 1.0 : t < 3.5 ? 1.0 - 0.7 * (t - 1.0) / 2.5 : 0.3; /* intro burst, then calm */
        render(now, level);

        if (shot && now - start > (Uint32)shot_after) {
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            break;
        }
        SDL_Delay(12);
    }
    if (child > 0) { kill(child, SIGHUP); waitpid(child, NULL, WNOHANG); }
    glyph_flush();
    term_free(term);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
