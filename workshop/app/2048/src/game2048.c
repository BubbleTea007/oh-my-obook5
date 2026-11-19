#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "atlas.h"

#define W 800
#define H 480
#define TILE 96
#define GAP 8
#define BOARD_X 15
#define BOARD_Y 28
#define BOARD_W (4 * TILE + 5 * GAP)
#define PANEL_X 460

#define C_WHITE 0xFFFF
#define C_BLACK 0x0000
#define C_G0 0xE71C
#define C_G1 0xC618
#define C_G2 0xA514
#define C_G3 0x8410
#define C_G4 0x630C
#define C_G5 0x4208
#define C_G6 0x2104

static int fb_fd = -1;
static unsigned char *fb_mem = NULL;
static size_t fb_size = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int LINE = 0;

static int board[4][4];
static int score = 0;
static int best = 0;
static int over = 0;
static int quit = 0;

static int last_x = 0, last_y = 0;
static int start_x = 0, start_y = 0;
static int touch_down = 0;
static int start_set = 0;

static void put_pixel(int x, int y, unsigned short c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    *(unsigned short *)(fb_mem + y * LINE + x * 2) = c;
}

static void fill_rect(int x, int y, int w, int h, unsigned short c) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w;
    int y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        unsigned short *p = (unsigned short *)(fb_mem + j * LINE + x0 * 2);
        for (int i = x0; i < x1; i++) *p++ = c;
    }
}

static void border(int x, int y, int w, int h, int t, unsigned short c) {
    fill_rect(x, y, w, t, c);
    fill_rect(x, y + h - t, w, t, c);
    fill_rect(x, y, t, h, c);
    fill_rect(x + w - t, y, t, h, c);
}

static int atlas_text_width(const char *s, const ObookGlyph *glyphs, int first, int count) {
    int w = 0;
    for (; *s; s++) {
        int idx = (unsigned char)*s - first;
        if (idx < 0 || idx >= count) idx = 0;
        w += glyphs[idx].adv;
    }
    return w;
}

static void draw_glyph(int x, int y, const ObookGlyph *g,
                       const unsigned char *bits, unsigned short color) {
    if (!g->w || !g->h) return;
    int stride = (g->w + 7) / 8;
    for (int row = 0; row < g->h; row++) {
        const unsigned char *rowbits = bits + g->off + row * stride;
        for (int col = 0; col < g->w; col++) {
            if (rowbits[col / 8] & (0x80 >> (col % 8)))
                fill_rect(x + g->xoff + col, y + g->yoff + row, 1, 1, color);
        }
    }
}

static int draw_atlas_text(int x, int y, const char *s,
                           const unsigned char *bits, const ObookGlyph *glyphs,
                           int first, int count, unsigned short color) {
    int x0 = x;
    for (; *s; s++) {
        int idx = (unsigned char)*s - first;
        if (idx < 0 || idx >= count) idx = 0;
        const ObookGlyph *g = &glyphs[idx];
        draw_glyph(x, y, g, bits, color);
        x += g->adv;
    }
    return x - x0;
}

static void draw_atlas_center(int cx, int y, const char *s,
                              const unsigned char *bits, const ObookGlyph *glyphs,
                              int first, int count, unsigned short color) {
    int w = atlas_text_width(s, glyphs, first, count);
    draw_atlas_text(cx - w / 2, y, s, bits, glyphs, first, count, color);
}

static void flip(void) {
    struct fb_var_screeninfo v = vinfo;
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &v);
    usleep(90000);
}

static unsigned short tile_bg(int v) {
    switch (v) {
        case 0: return C_G0;
        case 2: return C_WHITE;
        case 4: return 0xEF7D;
        case 8: return C_G1;
        case 16: return C_G2;
        case 32: return C_G3;
        case 64: return C_G4;
        case 128: return C_G5;
        case 256: return C_G6;
        default: return C_BLACK;
    }
}

static unsigned short tile_fg(int v) {
    return v >= 64 ? C_WHITE : C_BLACK;
}

static void draw_tile(int r, int c) {
    int x = BOARD_X + c * (TILE + GAP);
    int y = BOARD_Y + r * (TILE + GAP);
    int v = board[r][c];
    fill_rect(x, y, TILE, TILE, tile_bg(v));
    border(x, y, TILE, TILE, 2, C_BLACK);
    if (v) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", v);
        int digits = (int)strlen(buf);
        const unsigned char *bits;
        const ObookGlyph *glyphs;
        if (digits <= 2) {
            bits = digits48_bits;
            glyphs = digits48_glyphs;
        } else {
            bits = digits36_bits;
            glyphs = digits36_glyphs;
        }
        const ObookGlyph *g0 = &glyphs[buf[0] - DIGIT_FIRST];
        int tw = atlas_text_width(buf, glyphs, DIGIT_FIRST, DIGIT_COUNT);
        int tx = x + (TILE - tw) / 2;
        int ty = y + (TILE - g0->h) / 2 - g0->yoff;
        draw_atlas_text(tx, ty, buf, bits, glyphs, DIGIT_FIRST, DIGIT_COUNT, tile_fg(v));
    }
}

static void draw_button(int x, int y, int w, int h, const char *label, int big) {
    fill_rect(x, y, w, h, C_WHITE);
    border(x, y, w, h, 3, C_BLACK);
    const unsigned char *bits = big ? font36_bits : font22_bits;
    const ObookGlyph *glyphs = big ? font36_glyphs : font22_glyphs;
    int tw = atlas_text_width(label, glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT);
    const ObookGlyph *g0 = &glyphs[(unsigned char)label[0] - FONT_ASCII_FIRST];
    int tx = x + (w - tw) / 2;
    int ty = y + (h - g0->h) / 2 - g0->yoff;
    draw_atlas_text(tx, ty, label, bits, glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);
}

static void draw_game_over(void) {
    int x = BOARD_X - 6, y = BOARD_Y - 6;
    int w = BOARD_W + 12, h = BOARD_W + 12;
    fill_rect(x, y, w, h, C_WHITE);
    border(x, y, w, h, 5, C_BLACK);
    draw_atlas_center(x + w / 2, y + 80, "GAME OVER",
                      font36_bits, font36_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);
    draw_atlas_center(x + w / 2, y + 150, "SCORE",
                      font22_bits, font22_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", score);
    draw_atlas_center(x + w / 2, y + 178, buf,
                      digits48_bits, digits48_glyphs, DIGIT_FIRST, DIGIT_COUNT, C_BLACK);
    draw_atlas_center(x + w / 2, y + 320, "TAP TO RESTART",
                      font22_bits, font22_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);
}

static void draw_all(void) {
    fill_rect(0, 0, W, H, C_WHITE);

    /* board frame + tiles */
    fill_rect(BOARD_X - 5, BOARD_Y - 5, BOARD_W + 10, BOARD_W + 10, C_BLACK);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            draw_tile(r, c);

    /* title and score */
    draw_atlas_text(PANEL_X, 8, "2048",
                    digits48_bits, digits48_glyphs, DIGIT_FIRST, DIGIT_COUNT, C_BLACK);
    draw_atlas_text(PANEL_X, 70, "SCORE",
                    font22_bits, font22_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);
    draw_atlas_text(650, 70, "BEST",
                    font22_bits, font22_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", score);
    draw_atlas_text(PANEL_X, 94, buf,
                    digits36_bits, digits36_glyphs, DIGIT_FIRST, DIGIT_COUNT, C_BLACK);
    snprintf(buf, sizeof(buf), "%d", best);
    draw_atlas_text(650, 94, buf,
                    digits36_bits, digits36_glyphs, DIGIT_FIRST, DIGIT_COUNT, C_BLACK);

    draw_atlas_text(PANEL_X, 152, "SWIPE OR TAP",
                    font22_bits, font22_glyphs, FONT_ASCII_FIRST, FONT_ASCII_COUNT, C_BLACK);

    /* direction pad */
    draw_button(570, 195, 60, 48, "^", 1);
    draw_button(570, 312, 60, 48, "v", 1);
    draw_button(497, 252, 60, 48, "<", 1);
    draw_button(643, 252, 60, 48, ">", 1);

    /* bottom buttons */
    draw_button(470, 408, 140, 52, "NEW", 0);
    draw_button(630, 408, 140, 52, "EXIT", 0);

    if (over) draw_game_over();
    flip();
}

static void save_best(void) {
    FILE *f = fopen("/user/workshop/app/2048/data/best", "w");
    if (f) { fprintf(f, "%d\n", best); fclose(f); }
}

static void load_best(void) {
    FILE *f = fopen("/user/workshop/app/2048/data/best", "r");
    if (f) { if (fscanf(f, "%d", &best) != 1) best = 0; fclose(f); }
}

static void add_random(void) {
    int empty = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!board[r][c]) empty++;
    if (!empty) return;
    int pick = rand() % empty;
    int val = (rand() % 10 == 0) ? 4 : 2;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!board[r][c]) {
                if (pick-- == 0) { board[r][c] = val; return; }
            }
}

static int can_move(void) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!board[r][c]) return 1;
            if (c < 3 && board[r][c] == board[r][c + 1]) return 1;
            if (r < 3 && board[r][c] == board[r + 1][c]) return 1;
        }
    return 0;
}

static void cell_for(int dir, int i, int j, int *r, int *c) {
    if (dir == 0) { *r = j; *c = i; }
    else if (dir == 1) { *r = 3 - j; *c = i; }
    else if (dir == 2) { *r = i; *c = j; }
    else { *r = i; *c = 3 - j; }
}

static int do_move(int dir) {
    if (over) return 0;
    int moved = 0;
    for (int i = 0; i < 4; i++) {
        int line[4] = {0, 0, 0, 0}, n = 0;
        for (int j = 0; j < 4; j++) {
            int r, c;
            cell_for(dir, i, j, &r, &c);
            if (board[r][c]) line[n++] = board[r][c];
        }
        int out[4] = {0, 0, 0, 0}, k = 0;
        for (int j = 0; j < n; j++) {
            if (j + 1 < n && line[j] == line[j + 1]) {
                out[k] = line[j] * 2;
                score += out[k];
                j++;
            } else {
                out[k] = line[j];
            }
            k++;
        }
        for (int j = 0; j < 4; j++) {
            int r, c;
            cell_for(dir, i, j, &r, &c);
            if (board[r][c] != out[j]) moved = 1;
            board[r][c] = out[j];
        }
    }
    if (!moved) return 0;
    add_random();
    if (!can_move()) over = 1;
    if (score > best) { best = score; save_best(); }
    draw_all();
    return 1;
}

static void new_game(void) {
    memset(board, 0, sizeof(board));
    score = 0;
    over = 0;
    add_random();
    add_random();
    draw_all();
}

static void tap(int x, int y) {
    if (x >= 630 && x <= 770 && y >= 408 && y <= 460) { quit = 1; return; }
    if (x >= 470 && x <= 610 && y >= 408 && y <= 460) { new_game(); return; }
    if (over) {
        if (x >= BOARD_X && x <= BOARD_X + BOARD_W && y >= BOARD_Y && y <= BOARD_Y + BOARD_W)
            new_game();
        return;
    }
    if (x >= 570 && x <= 630 && y >= 195 && y <= 243) { do_move(0); return; }
    if (x >= 570 && x <= 630 && y >= 312 && y <= 360) { do_move(1); return; }
    if (x >= 497 && x <= 557 && y >= 252 && y <= 300) { do_move(2); return; }
    if (x >= 643 && x <= 703 && y >= 252 && y <= 300) { do_move(3); return; }
    /* tap on the board does nothing */
}

static void swipe(int dx, int dy) {
    if (over) return;
    if (abs(dx) > abs(dy)) {
        if (dx > 35) do_move(3);
        else if (dx < -35) do_move(2);
    } else {
        if (dy > 35) do_move(1);
        else if (dy < -35) do_move(0);
    }
}

static void handle_event(struct input_event *ev) {
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_MT_POSITION_X || ev->code == ABS_X) last_x = ev->value;
        else if (ev->code == ABS_MT_POSITION_Y || ev->code == ABS_Y) last_y = ev->value;
        if (touch_down && !start_set) {
            start_x = last_x;
            start_y = last_y;
            start_set = 1;
        }
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        if (ev->value) {
            touch_down = 1;
            start_set = 0;
            start_x = last_x;
            start_y = last_y;
        } else if (touch_down) {
            int dx = last_x - start_x;
            int dy = last_y - start_y;
            touch_down = 0;
            if (abs(dx) < 35 && abs(dy) < 35) tap(last_x, last_y);
            else swipe(dx, dy);
            start_set = 0;
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    srand((unsigned)time(NULL));
    unsigned char *saved = NULL;
    int saved_yoffset = 0;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { perror("open fb0"); return 1; }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) { perror("FBIOGET_VSCREENINFO"); return 1; }
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) { perror("FBIOGET_FSCREENINFO"); return 1; }
    LINE = finfo.line_length;
    fb_size = finfo.smem_len;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { perror("mmap fb"); return 1; }

    saved_yoffset = vinfo.yoffset;
    saved = malloc(fb_size);
    if (saved) memcpy(saved, fb_mem, fb_size);

    printf("2048: fb %ux%u bpp=%u line=%d\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, LINE);

    int ifd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (ifd < 0) { perror("open event1"); return 1; }
    ioctl(ifd, EVIOCGRAB, 1);

    load_best();
    printf("best=%d\n", best);
    new_game();

    while (!quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ifd, &rfds);
        struct timeval tv = {1, 0};
        int sr = select(ifd + 1, &rfds, NULL, NULL, &tv);
        if (sr <= 0) continue;
        struct input_event ev;
        while (read(ifd, &ev, sizeof(ev)) == sizeof(ev))
            handle_event(&ev);
    }

    save_best();
    printf("exit score=%d best=%d\n", score, best);
    ioctl(ifd, EVIOCGRAB, 0);
    if (saved) {
        memcpy(fb_mem, saved, fb_size);
        struct fb_var_screeninfo v = vinfo;
        v.yoffset = saved_yoffset;
        v.activate = FB_ACTIVATE_NOW;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &v);
        usleep(90000);
        free(saved);
        saved = NULL;
    }
    close(ifd);
    munmap(fb_mem, fb_size);
    close(fb_fd);
    return 0;
}
