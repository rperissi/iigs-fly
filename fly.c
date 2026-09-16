/*
 * FLY - IIgs 40th anniversary connectome viewer.
 * Spec: fly-demo-spec.md  (do not redesign)
 *
 * Step 2-4: labels + bench (chrome persists), title cards, disk boot.
 *
 *   occ -b -O255 -w255 fly.c -o fly
 *   iix chtyp -t s16 fly
 */
#include <stdio.h>
#include <string.h>
#include <orca.h>
#include <misctool.h>
#include <Memory.h>
#include "sin88.h"
#include "font640.h"    /* Geneva 9 + Monaco 9, hinted, for 640-mode text rows */
#ifndef NO_CARD
#include "cogs.h"        /* cogslib: card presence, STATUS, HTTP, TLS_INFO */
#include "cogs_io_slot.h"
#else
#include "cogs.h"
#include "cogs_io_slot.h"
#endif
#include "music.h"

#define PIXDST   ((unsigned char *)0x00E12000L)
#define SCBDST   ((unsigned char *)0x00E19D00L)
#define PALDST   ((unsigned char *)0x00E19E00L)
#define NEWVIDEO ((unsigned char *)0x00C029L)
#define BORDERREG ((unsigned char *)0x00C034L)
#define KBD      ((unsigned char *)0x00C000L)
#define STROBE   ((unsigned char *)0x00C010L)

#define SHR_SAVE  0x8000L
#define VP_Y0     10
#define VP_Y1     146        /* rows 147-199: boxed data block + credit */
#define VP_CX     160
#define VP_CY     78
#define TXT_Y0    147
/* data box: rule at 147, four 9-row lines from 150, rule at 187 */
#define BOX_TOP   147
#define BOX_BOT   187
#define L_LABEL1  150
#define L_LABEL2  159
#define L_BENCH1  168
#define L_BENCH2  177
#define L_CREDIT  190
#define RAMP_MAX  12         /* depth ramp 1..12; 13 grid, 14 terminals, 15 soma */
#define GRID_C    13
/* faint wireframe grid behind the neuron: every 32 px / 32 rows, offset 16 */
#define ON_GRID(x, y) 0   /* grid off (Rob, 9:41 PM); erase paths still honor it */
#define MAX_NODES 600
#define COL_HEAD  15
#define COL_BODY  13
#define CONNECTOME 166700UL
#define DWELL     (12UL * 60UL)
#define SCB_PAL1  0x01       /* SCB palette is the low nibble */
#define SCB_TEXT  0x81       /* 640 mode + palette 1: text rows */
#define BOOT_Y0   96         /* boot console band, 640 mode, rows 96-199 */
/* The Fly.shr: index 14 is $000 in every palette and index 15 is unused,
   so text over the poster uses 14 as black backing and 13/15 as colors
   after blit_fly_shr() folds the poster's 13s (near-black) into 14. */
#define BOOT_BLACK 14

/* Depth ramp, far (1) deep blue to near (13) bright aqua, $0RGB. Entry 14
   is the branch-terminal accent, 15 the soma. Per-neuron hue variants are
   channel permutations of 1..13 (teal, magenta, green). */
static const unsigned RAMP[16] = {
    0x000, 0x113, 0x124, 0x136, 0x147, 0x158, 0x279, 0x28A,
    0x39B, 0x4AC, 0x6BD, 0x8CE, 0x9EE, 0x333, 0xFB5, 0xFFF
};
static int g_hue;

typedef struct {
    const char *pref;
    const char *tag;
} TypeTag;

static const TypeTag TAGS[] = {
    {"GF",     "giant fiber, the escape reflex"},
    {"DNa01",  "descending neuron, brain to nerve cord, steering"},
    {"DNa02",  "descending neuron, brain to nerve cord, turning"},
    {"MBON01", "mushroom body output, memory readout"},
    {"MBON03", "mushroom body output, memory readout"},
    {"LC10",   "visual neuron, tracks a mate during courtship"},
    {"LC4",    "visual neuron, detects looming threats"},
    {"PPL101", "dopamine neuron, reward and punishment learning"},
    {"aMe12",  "circadian clock neuron"},
    {0, 0}
};

typedef struct {
    unsigned long bodyId;
    char type[16];
    unsigned nNodes;
    unsigned long pre, post;
    int *xyz;
    unsigned *parent;
} Neuron;

static unsigned char *g_save;
static unsigned char g_videomode, g_border;
static unsigned char *g_file;
static unsigned long g_flen;
static Handle g_fh, g_sh;
static Neuron g_nr[32];
static unsigned g_ncount;
static int g_sx[MAX_NODES], g_sy[MAX_NODES];
static int g_nsx[MAX_NODES], g_nsy[MAX_NODES];
static unsigned char g_nc[MAX_NODES];
static int g_haveprev;
static int g_xdiv = 200, g_ydiv = 180;
static unsigned g_cur;
static unsigned long g_N, g_S;
static unsigned long g_t0, g_nr0, g_secmark;
static unsigned g_frames, g_nodesacc, g_fps, g_nps;
static int g_auto = 1;
static int g_needtext = 1;
static int g_chrome = 0;
static int g_relabel = 1;
static int g_mutx = -1, g_mutw;   /* 640-mode mute slot on the header row */
static int g_cx, g_cy;
static int g_bootdraw;   /* 1 = console cursor in 640 mode */
static int g_escape;     /* Esc seen mid-frame or mid-download */

/* ---- CoGS card path (spec section 4, step 5) ---- */
#define FLY_URL "https://a2cogs.com/fly/GSFLY.DATA"
static cogs      g_c;
static cogs_io   g_io;
static cogs_slot g_slot;
static int       g_cslot;             /* 0 = no card / no link / probe failed */
static cogs_tls_reply g_tls;
static unsigned char g_probe[32];     /* first 32 bytes via a Range GET */
static unsigned  g_probe_len;
static unsigned long g_netbytes;      /* bytes received over the card */
static unsigned char *g_rowp[200];   /* row start pointers, no long multiply per pixel */

static unsigned rd16(unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(unsigned char *p)
{
    return (unsigned long)p[0]
         | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[3] << 24);
}

static int getkey(void)
{
    int k;
    if (!(KBD[0] & 0x80)) return -1;
    k = KBD[0] & 0x7F;
    STROBE[0] = 0;
    return k;
}

static const char *tag_for(const char *typ)
{
    const TypeTag *t;
    for (t = TAGS; t->pref; t++) {
        unsigned n = (unsigned)strlen(t->pref);
        if (strncmp(typ, t->pref, n) == 0) return t->tag;
    }
    return "brain neuron";
}

static void pix(int x, int y, unsigned char c)
{
    unsigned char *p;
    if (x < 0 || x > 319 || y < 0 || y > 199) return;
    p = g_rowp[y] + (x >> 1);
    if (x & 1) *p = (unsigned char)((*p & 0xF0) | (c & 0x0F));
    else       *p = (unsigned char)((*p & 0x0F) | ((c & 0x0F) << 4));
}

static void setpix(int x, int y, unsigned char c)
{
    if (y < VP_Y0 || y > VP_Y1) return;
    pix(x, y, c);
}

static void setpal(int pal, int idx, unsigned rgb);

/* ---- 640-mode text rows: 4 pixels per byte, leftmost in bits 7-6 ---- */
static void pix640(int x, int y, unsigned char v)
{
    unsigned char *p;
    int sh;
    if (x < 0 || x > 639 || y < 0 || y > 199) return;
    p = g_rowp[y] + (x >> 2);
    sh = (3 - (x & 3)) * 2;
    *p = (unsigned char)((*p & ~(3 << sh)) | ((v & 3) << sh));
}

typedef struct {
    const unsigned char *adv;
    const unsigned *full, *mid;
    int rows;
} Font640;

/* Filled in at run time by fonts_init(). Static initializers holding the
   address of another static array relocate wrong once the link has more
   than one object file (the cogslib objects): the console came up as
   scattered strokes. Pointers to string literals are fine. */
static Font640 FMONO;
static Font640 FPROP;
static void fonts_init(void)
{
    FMONO.adv = FM_ADV; FMONO.full = FM_FULL; FMONO.mid = FM_MID; FMONO.rows = FM_ROWS;
    FPROP.adv = FP_ADV; FPROP.full = FP_FULL; FPROP.mid = FP_MID; FPROP.rows = FP_ROWS;
}

/* 640 palette quads: level 0 black, 1 dim aqua (AA edge), 2 aqua body,
   3 white-green headline. Same in all four quads so position is moot. */
static void set_textpal(int pal, int t, int n)
{
    static const unsigned LV[4] = { 0x000, 0x333, 0x6EE, 0xCFF };   /* 1 = dark gray: rules, box, corner text */
    int e;
    for (e = 0; e < 16; e++) {
        unsigned rgb = LV[e & 3];
        unsigned r = ((rgb >> 8) & 15) * t / n;
        unsigned g = ((rgb >> 4) & 15) * t / n;
        unsigned b = (rgb & 15) * t / n;
        setpal(pal, e, (r << 8) | (g << 4) | b);
    }
}

/* Hinted 9px bitmap (Geneva / Monaco); each source column is two 640
   pixels. head = level 3, body = level 2. clear: black the cell first. */
/* head: 0 body (level 2 aqua), 1 headline (level 3), 2 gray (level 1) */
static int dc640(const Font640 *f, int x, int y, char ch, int head, int clear)
{
    int i, r, c, adv, ncol, low, low0;
    unsigned fm;
    unsigned char nib = (head == 1) ? 0x0F : (head == 2) ? 0x05 : 0x0A;
    unsigned char *p;
    if (ch < 32 || ch > 126) ch = ' ';
    i = ch - 32;
    adv = f->adv[i];
    /* x is always even here: one source column is one 4-bit nibble */
    ncol = 12;
    if (x + ncol * 2 > 640) ncol = (640 - x) / 2;
    low0 = (x >> 1) & 1;
    for (r = 0; r < f->rows; r++) {
        if (y + r > 199) break;
        fm = f->full[i * f->rows + r];
        p = g_rowp[y + r] + (x >> 2);
        low = low0;
        for (c = 0; c < ncol; c++) {
            if (fm & (0x8000u >> c)) {
                if (low) *p = (unsigned char)((*p & 0xF0) | nib);
                else     *p = (unsigned char)((*p & 0x0F) | (nib << 4));
            } else if (clear && c < adv) {
                if (low) *p &= 0xF0;
                else     *p &= 0x0F;
            }
            if (low) p++;
            low ^= 1;
        }
    }
    return x + adv * 2;
}

static int w640(const Font640 *f, const char *s)
{
    int w = 0;
    while (*s) {
        char ch = *s++;
        if (ch < 32 || ch > 126) ch = ' ';
        w += f->adv[ch - 32] * 2;
    }
    return w;
}

static int text640(const Font640 *f, int x, int y, const char *s, int head, int clear)
{
    while (*s) x = dc640(f, x, y, *s++, head, clear);
    return x;
}

static void clear_640_span(int x, int y, int w, int h)
{
    int i, r;
    for (r = 0; r < h; r++)
        for (i = 0; i < w; i++)
            pix640(x + i, y + r, 0);
}

/* Swap "M mute" / "U unmute" in the reserved header slot. No chrome rebuild. */
static void draw_mute(void)
{
    if (g_mutx < 0 || !music_ok()) return;
    clear_640_span(g_mutx, 0, g_mutw, FPROP.rows);
    text640(&FPROP, g_mutx, 0, music_muted() ? "U unmute" : "M mute", 2, 0);
}

/* Bresenham into the viewport. Pixels are written inline; the clip test
   runs per pixel only when an endpoint is outside the viewport. */
static void line(int x0, int y0, int x1, int y1, unsigned char c)
{
    int dx, dy, sx, sy, err, e2, clip;
    unsigned char *p, hi = (unsigned char)(c << 4);
    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    clip = (x0 < 0 || x0 > 319 || x1 < 0 || x1 > 319 ||
            y0 < VP_Y0 || y0 > VP_Y1 || y1 < VP_Y0 || y1 > VP_Y1);
    for (;;) {
        if (!clip || (x0 >= 0 && x0 <= 319 && y0 >= VP_Y0 && y0 <= VP_Y1)) {
            p = g_rowp[y0] + (x0 >> 1);
            if (c == 0 && ON_GRID(x0, y0)) {   /* erasing: put the grid back */
                if (x0 & 1) *p = (unsigned char)((*p & 0xF0) | GRID_C);
                else        *p = (unsigned char)((*p & 0x0F) | (GRID_C << 4));
            } else if (x0 & 1) *p = (unsigned char)((*p & 0xF0) | c);
            else               *p = (unsigned char)((*p & 0x0F) | hi);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void clear_rows(int y0, int y1)
{
    int y, i;
    unsigned char *p;
    for (y = y0; y <= y1; y++) {
        p = PIXDST + (long)y * 160L;
        for (i = 0; i < 160; i++) p[i] = 0;
    }
}

static void setpal(int pal, int idx, unsigned rgb)
{
    unsigned r = (rgb >> 8) & 15;
    unsigned g = (rgb >> 4) & 15;
    unsigned b = rgb & 15;
    unsigned char *p = PALDST + (long)pal * 32L + (long)idx * 2L;
    p[0] = (unsigned char)((g << 4) | b);
    p[1] = (unsigned char)r;
}

static void save_desktop(void)
{
    long i;
    g_videomode = *NEWVIDEO;
    g_border = *BORDERREG;
    for (i = 0; i < SHR_SAVE; i++) g_save[i] = PIXDST[i];
}

static void restore_desktop(void)
{
    long i;
    *NEWVIDEO = (unsigned char)(g_videomode & 0x7F);
    for (i = 0; i < SHR_SAVE; i++) PIXDST[i] = g_save[i];
    *NEWVIDEO = g_videomode;
    *BORDERREG = (unsigned char)((*BORDERREG & 0xF0) | (g_border & 0x0F));
}

static void shr_on_black(void)
{
    long i;
    *NEWVIDEO |= 0x80;
    *BORDERREG &= 0xF0;
    for (i = 0; i < 512L; i++) PALDST[i] = 0;
    for (i = 0; i < 200L; i++) {
        if (i < VP_Y0 || i >= TXT_Y0) SCBDST[i] = SCB_TEXT; /* viewer chrome, 640 */
        else SCBDST[i] = 0;
    }
    for (i = 0; i < 32000L; i++) PIXDST[i] = 0;
}

static unsigned hue_perm(unsigned rgb, int v)
{
    unsigned r = (rgb >> 8) & 15, g = (rgb >> 4) & 15, b = rgb & 15;
    switch (v % 3) {
    case 1:  return (b << 8) | (r << 4) | g;   /* magenta / violet */
    case 2:  return (r << 8) | (b << 4) | g;   /* green */
    default: return rgb;                       /* teal / blue */
    }
}

static void install_ramp(void)
{
    int i;
    setpal(0, 0, 0x000);
    for (i = 1; i <= RAMP_MAX; i++) setpal(0, i, hue_perm(RAMP[i], g_hue));
    setpal(0, GRID_C, RAMP[GRID_C]);
    setpal(0, 14, RAMP[14]);
    setpal(0, 15, RAMP[15]);
    set_textpal(1, 1, 1);
}

static void draw_grid(void)
{
    /* wireframe grid removed; ON_GRID is 0 so this is a no-op */
}

static FILE *open_data(void)
{
    static const char *paths[] = {
        "1/GSFLY.DATA",
        "GSFLY.DATA",
        "/GSFLY/GSFLY/GSFLY.DATA",   /* boot volume GSFLY, folder GSFLY */
        "/GSFLY800/GSFLY.DATA",      /* 800K floppy, volume GSFLY800 */
        "/GSFLY/GSFLY.DATA",         /* older 800K gold image */
        0
    };
    FILE *f;
    int i;
    for (i = 0; paths[i]; i++) {
        f = fopen(paths[i], "rb");
        if (f) return f;
    }
    return 0;
}

static int load_file(void)
{
    FILE *f;
    unsigned long got = 0;
    size_t n;
    f = open_data();
    if (!f) { printf("cannot open FLYDATA.BIN\n"); return 0; }
    if (fseek(f, 0L, 2) != 0) { fclose(f); printf("seek fail\n"); return 0; }
    g_flen = (unsigned long)ftell(f);
    rewind(f);
    if (g_flen < 8L || g_flen > 400000L) {
        fclose(f); printf("bad size %lu\n", g_flen); return 0;
    }
    g_fh = NewHandle(g_flen, userid(), attrLocked | attrFixed, 0L);
    if (toolerror()) { fclose(f); printf("NewHandle file %04x\n", toolerror()); return 0; }
    g_file = (unsigned char *)*g_fh;
    while (got < g_flen) {
        n = fread(g_file + got, 1, (size_t)(g_flen - got), f);
        if (!n) break;
        got += (unsigned long)n;
    }
    fclose(f);
    if (got != g_flen) { printf("short read %lu\n", got); return 0; }
    return 1;
}

static int parse_block(void)
{
    unsigned char *p, *end;
    unsigned i, n;
    if (g_flen < 6L) return 0;
    if (g_file[0] != 'F' || g_file[1] != 'L' || g_file[2] != 'Y' || g_file[3] != '1') {
        printf("bad magic\n");
        return 0;
    }
    g_ncount = rd16(g_file + 4);
    if (g_ncount == 0 || g_ncount > 32) { printf("count %u\n", g_ncount); return 0; }
    p = g_file + 6;
    end = g_file + g_flen;
    for (i = 0; i < g_ncount; i++) {
        Neuron *nr = &g_nr[i];
        if (p + 26 > end) return 0;
        nr->bodyId = rd32(p); p += 4;
        memcpy(nr->type, p, 16); p += 16;
        nr->type[15] = 0;
        n = rd16(p); p += 2;
        nr->nNodes = n;
        nr->pre = rd32(p); p += 4;
        nr->post = rd32(p); p += 4;
        if (n == 0 || n > MAX_NODES) return 0;
        if (p + (unsigned long)n * 8L > end) return 0;
        nr->xyz = (int *)p;
        p += (unsigned long)n * 6L;
        nr->parent = (unsigned *)p;
        p += (unsigned long)n * 2L;
    }
    return 1;
}

/* Per-neuron working copy in screen units, |x|,|z| <= W_MAX, |y| <= 57.
   With a 7-bit sine (max 128) the rotation products stay inside 16 bits,
   so the per-frame transform is all int math. The long divides happen
   once here, when a neuron comes up, not 600 times a frame. */
#define W_MAX 126
static int g_w[MAX_NODES * 3];
static void choose_scale(Neuron *nr)
{
    unsigned k;
    int mx = 1, my = 1, a;
    long xd, yd;
    for (k = 0; k < nr->nNodes; k++) {
        a = nr->xyz[k * 3];     if (a < 0) a = -a; if (a > mx) mx = a;
        a = nr->xyz[k * 3 + 2]; if (a < 0) a = -a; if (a > mx) mx = a;
        a = nr->xyz[k * 3 + 1]; if (a < 0) a = -a; if (a > my) my = a;
    }
    xd = mx / W_MAX; if (xd < 1) xd = 1;
    yd = my / 62;    if (yd < 1) yd = 1;
    for (k = 0; k < nr->nNodes; k++) {
        g_w[k * 3]     = (int)((long)nr->xyz[k * 3] / xd);
        g_w[k * 3 + 1] = (int)((long)nr->xyz[k * 3 + 1] / yd);
        g_w[k * 3 + 2] = (int)((long)nr->xyz[k * 3 + 2] / xd);
    }
}

/* zr is in -W_MAX..W_MAX after the 7-bit rotation */
static unsigned char zcolor(int zr)
{
    int c = ((zr + W_MAX) * RAMP_MAX) / (2 * W_MAX + 1) + 1;
    if (c < 1) c = 1;
    if (c > RAMP_MAX) c = RAMP_MAX;
    return (unsigned char)c;
}

/* Leaves (no children) get the terminal accent; roots get the soma mark. */
static unsigned char g_leaf[MAX_NODES];
static void mark_leaves(Neuron *nr)
{
    unsigned k, par;
    for (k = 0; k < nr->nNodes; k++) g_leaf[k] = 1;
    for (k = 0; k < nr->nNodes; k++) {
        par = nr->parent[k];
        if (par < nr->nNodes) g_leaf[par] = 0;
    }
}

static void spix(int x, int y, unsigned char c)
{
    if (c == 0 && ON_GRID(x, y)) c = GRID_C;
    setpix(x, y, c);
}

static void soma(int x, int y, unsigned char c)
{
    int i, j;
    for (j = -1; j <= 1; j++)
        for (i = -1; i <= 1; i++)
            if (i == 0 || j == 0) spix(x + i, y + j, c);
    spix(x - 2, y, c); spix(x + 2, y, c);
    spix(x, y - 2, c); spix(x, y + 2, c);
}

/* 16-bit rotation about Y with a 7-bit sine; products <= 126*128*2 */
static void project_all(Neuron *nr, int ang)
{
    int s = SIN88[ang & 255] >> 1;
    int c = COS88(ang) >> 1;
    int *w = g_w;
    unsigned k;
    for (k = 0; k < nr->nNodes; k++, w += 3) {
        int x = w[0], z = w[2];
        g_nsx[k] = VP_CX + ((x * c - z * s) >> 7);
        g_nsy[k] = VP_CY + w[1];
        g_nc[k]  = zcolor((x * s + z * c) >> 7);
    }
}

static void draw_neuron(Neuron *nr, int ang)
{
    unsigned k, par;
    project_all(nr, ang);
    /* Erase and redraw segment by segment rather than whole passes, so at
       one or two frames a second the neuron never blanks out between the
       erase pass and the draw pass. An old segment crossing a new one can
       nick a pixel; the next frame repaints it. */
    for (k = 0; k < nr->nNodes; k++) {
        /* a frame is most of a second: catch Esc mid-frame */
        if ((k & 63) == 63 && (KBD[0] & 0x80) && (KBD[0] & 0x7F) == 0x1B) {
            g_escape = 1;
            return;
        }
        par = nr->parent[k];
        if (g_haveprev) {
            if (par >= nr->nNodes) soma(g_sx[k], g_sy[k], 0);
            else line(g_sx[k], g_sy[k], g_sx[par], g_sy[par], 0);
        }
        if (par < nr->nNodes)
            line(g_nsx[k], g_nsy[k], g_nsx[par], g_nsy[par], g_nc[k]);
        g_sx[k] = g_nsx[k];
        g_sy[k] = g_nsy[k];
    }
    /* accents on top of the wire: terminals in 14, soma in 15 */
    for (k = 0; k < nr->nNodes; k++) {
        if (nr->parent[k] >= nr->nNodes) soma(g_nsx[k], g_nsy[k], 15);
        else if (g_leaf[k]) setpix(g_nsx[k], g_nsy[k], 14);
    }
    g_haveprev = 1;
}

#define BENCH_CELLS 8
static void bar16(char *b, unsigned long n, unsigned long d)
{
    unsigned i, f;
    b[0] = '[';
    f = 0;
    if (d) {
        f = (unsigned)((n * BENCH_CELLS) / d);
        if (f > BENCH_CELLS) f = BENCH_CELLS;
    }
    for (i = 0; i < BENCH_CELLS; i++) b[1 + i] = (char)(i < f ? '#' : '.');
    b[1 + BENCH_CELLS] = ']';
    b[2 + BENCH_CELLS] = 0;
}

/* Redraw a data line in place: glyph cells clear as they draw, then the
   tail of the line is blacked, so nothing blanks out between frames. */
static void line640(int x, int y, const char *s, int head)
{
    int xe = text640(&FPROP, x, y, s, head, 1);
    int r;
    unsigned char *p;
    for (r = 0; r < FP_ROWS; r++) {
        int xx = xe;
        p = g_rowp[y + r] + (xx >> 2);
        if (xx & 2) { *p &= 0xF0; p++; xx += 2; }
        while (xx < 636) { *p++ = 0; xx += 4; }
    }
}

/* Narrow: one 640 pixel per source column, half the width of line text. */
static int text640n(int x, int y, const char *s, int head)
{
    int i, r, c, adv;
    unsigned fm;
    unsigned char lv = head ? 3 : 2;
    while (*s) {
        char ch = *s++;
        if (ch < 32 || ch > 126) ch = ' ';
        i = ch - 32;
        adv = FP_ADV[i];
        for (r = 0; r < FP_ROWS; r++) {
            fm = FP_FULL[i * FP_ROWS + r];
            for (c = 0; c < 12; c++)
                if (fm & (0x8000u >> c)) pix640(x + c, y + r, lv);
        }
        x += adv;
    }
    return x;
}

static int w640n(const char *s)
{
    int w = 0;
    while (*s) {
        char ch = *s++;
        if (ch < 32 || ch > 126) ch = ' ';
        w += FP_ADV[ch - 32];
    }
    return w;
}

static void hrule(int y, int x0, int x1)
{
    int x;
    for (x = x0; x <= x1; x++) pix640(x, y, 1);
}

static void draw_labels(Neuron *nr)
{
    char buf[80];
    sprintf(buf, "Neuron %s: %s", nr->type, tag_for(nr->type));
    line640(8, L_LABEL1, buf, 1);
    sprintf(buf, "ID %lu   %u nodes   %lu pre / %lu post synapses",
            nr->bodyId, nr->nNodes, nr->pre, nr->post);
    line640(8, L_LABEL2, buf, 0);
}

static void draw_text(Neuron *nr, unsigned long now)
{
    char buf[80], br[20];
    unsigned long sec, tenths, days, hours;
    unsigned i;

    if (!g_chrome) {
        static const char title[] = "Apple IIgs Fruit Fly Brain Mapping";
        static const char ver[] = "v1.1 2026";
        static const char esc[] = "Esc to exit";
        int y, vx, ex, tw;
        clear_rows(0, 9);
        clear_rows(TXT_Y0, 199);
        tw = w640(&FPROP, title);
        text640(&FPROP, 4, 0, title, 1, 0);
        vx = (636 - w640(&FPROP, ver)) & ~1;
        text640(&FPROP, vx, 0, ver, 2, 0);
        ex = (vx - 16 - w640(&FPROP, esc)) & ~1;
        text640(&FPROP, ex, 0, esc, 2, 0);
        g_mutx = -1;
        if (music_ok()) {
            g_mutw = w640(&FPROP, "U unmute");
            g_mutx = (ex - 12 - g_mutw) & ~1;
            if (g_mutx < 4 + tw + 8) g_mutx = (4 + tw + 8) & ~1;
            draw_mute();
        }
        hrule(9, 2, 637);
        /* data box */
        hrule(BOX_TOP, 2, 637);
        hrule(BOX_BOT, 2, 637);
        for (y = BOX_TOP; y <= BOX_BOT; y++) { pix640(2, y, 1); pix640(3, y, 1); pix640(636, y, 1); pix640(637, y, 1); }
        text640(&FPROP, 4, L_CREDIT, "MaleCNS v1.0  HHMI Janelia / Google Research   CoGS a2cogs.com", 2, 0);
        g_chrome = 1;
        g_relabel = 1;
    }
    if (g_relabel) {
        draw_labels(nr);
        g_relabel = 0;
    }

    i = g_cur + 1;
    sprintf(buf, "%u nodes/sec   %u fps   neuron %u/%u",
            g_nps, g_fps, i, g_ncount);
    line640(8, L_BENCH1, buf, 0);

    sec = (now - g_t0) / 60UL;
    tenths = (g_N * 1000UL) / CONNECTOME;
    bar16(br, g_N, CONNECTOME);
    if (g_N > 0 && ((sec / 5UL) & 1) && sec > 0) {
        days = (CONNECTOME * sec) / (g_N * 86400UL);
        hours = (CONNECTOME * sec) / (g_N * 3600UL);
        if (days < 1UL)
            sprintf(buf, "At this rate: full connectome in %lu hours", hours);
        else
            sprintf(buf, "At this rate: full connectome in %lu days", days);
    } else {
        sprintf(buf, "Done: %lu neurons  %lu synapses  %s %lu.%lu%% of 166,700",
                g_N, g_S, br, tenths / 10UL, tenths % 10UL);
    }
    line640(8, L_BENCH2, buf, 0);
}

static void advance(void)
{
    Neuron *nr = &g_nr[g_cur];
    g_N++;
    g_S += nr->pre + nr->post;
    g_cur++;
    if (g_cur >= g_ncount) g_cur = 0;
    choose_scale(&g_nr[g_cur]);
    mark_leaves(&g_nr[g_cur]);
    g_haveprev = 0;
    clear_rows(VP_Y0, VP_Y1);
    draw_grid();
    g_hue++;
    install_ramp();
    g_nr0 = GetTick();
    g_relabel = 1;
    g_needtext = 1;
}

static unsigned lerp_rgb(unsigned dst, int t, int n)
{
    unsigned r = (dst >> 8) & 15, g = (dst >> 4) & 15, b = dst & 15;
    if (t <= 0) return 0;
    if (t >= n) return dst;
    return ((r * t / n) << 8) | ((g * t / n) << 4) | (b * t / n);
}

static void cursor(int on)
{
    int i, j;
    if (g_bootdraw) {   /* 640-mode console block, one mono cell */
        for (j = 0; j < FM_ROWS; j++)
            for (i = 0; i < FM_ADV[0] * 2; i++)
                pix640(g_cx + i, g_cy + j, on ? 3 : 0);
        return;
    }
    for (j = 0; j < 7; j++)
        for (i = 0; i < 5; i++)
            pix(g_cx + i, g_cy + j, on ? COL_HEAD : 0);
}

/* 0=done, 1=Esc quit, 2=skip ahead */
static int wait_ticks(unsigned n)
{
    unsigned long t0, last, now;
    int blink = 1, k;
    t0 = last = GetTick();
    cursor(1);
    for (;;) {
        now = GetTick();
        if (now - t0 >= n) break;
        k = getkey();
        if (k == 0x1B) { cursor(0); return 1; }
        if (k >= 0) { cursor(0); return 2; }
        if (now - last >= 8UL) {
            blink = !blink;
            cursor(blink);
            last = now;
        }
    }
    cursor(0);
    return 0;
}

static int type_str(const char *s, unsigned char c)
{
    int r;
    int k;
    while (*s) {
        /* six glyphs per tick: ~3 ms a character on the 60 Hz clock */
        for (k = 0; k < 6 && *s; k++)
            g_cx = dc640(&FMONO, g_cx, g_cy, *s++, c == COL_HEAD, 1);
        r = wait_ticks(1);
        if (r) return r;
    }
    return 0;
}

/* 11 text lines x 9 + 5 blanks x 1 = 104 rows from BOOT_Y0 (96). */
#define BOOT_X0 8
#define HOLD_LINE 11   /* ~180 ms after a paragraph */
static void boot_nl(void)
{
    g_cy += 9;
    g_cx = BOOT_X0;
    if (g_cy > 191) g_cy = 191;
}

static void boot_blank(void)
{
    g_cy += 1;
    g_cx = BOOT_X0;
    if (g_cy > 191) g_cy = 191;
}

static unsigned file_blockid(void)
{
    unsigned long s = 0;
    unsigned i, n;
    n = 32;
    if ((unsigned long)n > g_flen) n = (unsigned)g_flen;
    for (i = 0; i < n; i++) s += g_file[i];
    return (unsigned)(s & 0xFFFF);
}

/* Raw $C1/0000 screen, 32768 bytes, into dst. name is the bare file name. */
static int load_shr(const char *name, unsigned char *dst)
{
    char path[40];
    static const char *pre[] = { "1/", "", "/GSFLY/GSFLY/", "/GSFLY800/", "/GSFLY/", 0 };
    FILE *f = 0;
    unsigned long got = 0;
    size_t n;
    int i;
    for (i = 0; pre[i]; i++) {
        sprintf(path, "%s%s", pre[i], name);
        f = fopen(path, "rb");
        if (f) break;
    }
    if (!f) return 0;
    while (got < 32768UL) {
        n = fread(dst + got, 1, (size_t)(32768UL - got), f);
        if (!n) break;
        got += (unsigned long)n;
    }
    fclose(f);
    return got == 32768UL;
}

/* Console layout: reduced poster in rows 0..BOOT_Y0-1 (320 mode, its own
   palettes), black 640-mode console on palette 1 below. Staged through a
   buffer with the palettes blacked first, so the picture appears in one
   step instead of showing new pixels through the old palette. */
static int blit_boot_shr(void)
{
    Handle h;
    unsigned char *buf;
    int i;
    h = NewHandle(32768L, userid(), attrLocked | attrFixed, 0L);
    if (toolerror()) return 0;
    buf = (unsigned char *)*h;
    if (!load_shr("GSFLY.SHR", buf)) { DisposeHandle(h); return 0; }
    memset(buf + (long)BOOT_Y0 * 160L, 0, (200 - BOOT_Y0) * 160);
    for (i = BOOT_Y0; i < 200; i++) buf[0x7D00L + i] = SCB_TEXT;
    memset(PALDST, 0, 512);
    memcpy(SCBDST, buf + 0x7D00L, 200);
    memcpy(PIXDST, buf, 32000);
    memcpy(PALDST, buf + 0x7E00L, 512);
    set_textpal(1, 1, 1);
    DisposeHandle(h);
    return 1;
}

/* ---- 640-mode title cards (GSFLY.CARDS from gen_cards.py) ---- */
#define CARD_BYTES 32200L    /* 32000 px (2bpp x 640) + 200 SCBs */
#define PAL_HEAD 1
#define PAL_BODY 2

/* 640 mode reads a different palette quad per pixel position; fill all
   four quads with the same 4-level ramp so position never matters. */
static void set640(int pal, unsigned rgb, int t, int n)
{
    unsigned r = (rgb >> 8) & 15, g = (rgb >> 4) & 15, b = rgb & 15;
    int e, lv;
    unsigned rr, gg, bb;
    for (e = 0; e < 16; e++) {
        lv = e & 3;
        rr = (r * lv * t) / (3 * n);
        gg = (g * lv * t) / (3 * n);
        bb = (b * lv * t) / (3 * n);
        setpal(pal, e, (rr << 8) | (gg << 4) | bb);
    }
}

static void fade_cards(int t, int n)
{
    set640(PAL_HEAD, 0xCFF, t, n);
    set640(PAL_BODY, 0x6EE, t, n);
}

static FILE *open_cards(void)
{
    static const char *paths[] = {
        "1/GSFLY.CARDS", "GSFLY.CARDS", "/GSFLY/GSFLY/GSFLY.CARDS",
        "/GSFLY800/GSFLY.CARDS", "/GSFLY/GSFLY.CARDS", 0
    };
    FILE *f;
    int i;
    for (i = 0; paths[i]; i++) {
        f = fopen(paths[i], "rb");
        if (f) return f;
    }
    return 0;
}

static int load_card(FILE *f, int ci, unsigned char *buf)
{
    unsigned long got = 0;
    size_t n;
    if (fseek(f, (long)ci * CARD_BYTES, 0) != 0) return 0;
    while (got < (unsigned long)CARD_BYTES) {
        n = fread(buf + got, 1, (size_t)(CARD_BYTES - (long)got), f);
        if (!n) break;
        got += (unsigned long)n;
    }
    return got == (unsigned long)CARD_BYTES;
}

/* 0 done, 1 Esc, 2 skipped, -1 no card file (caller falls back) */
#define CARD_FADE 8     /* ticks, ~130 ms each way */
static int title_cards_640(void)
{
#define NCARDS 3
    static const unsigned holds[NCARDS] = {360, 250, 280};   /* 6 / 4.2 / 4.7 s */
    FILE *f;
    Handle h;
    unsigned char *all, *buf;
    long i;
    int ci, t, r;

    unsigned long hold_t0;
    int loaded;

    f = open_cards();
    if (!f) return -1;
    h = NewHandle(CARD_BYTES * NCARDS, userid(), attrLocked | attrFixed, 0L);
    if (toolerror()) { fclose(f); return -1; }
    all = (unsigned char *)*h;
    /* Only the first card is read before anything shows. The rest load
       during card 1's hold, and the hold clock keeps running meanwhile. */
    if (!load_card(f, 0, all)) { fclose(f); DisposeHandle(h); return -1; }
    loaded = 1;

    shr_on_black();
    fade_cards(0, CARD_FADE);
    g_cx = 0;
    g_cy = 220;
    g_bootdraw = 0;
    r = 0;

    for (ci = 0; ci < NCARDS && r == 0; ci++) {
        buf = all + (long)ci * CARD_BYTES;
        memcpy(PIXDST, buf, 32000);
        memcpy(SCBDST, buf + 32000L, 200);
        for (t = 0; t <= CARD_FADE; t++) {
            fade_cards(t, CARD_FADE);
            r = wait_ticks(1);
            if (r) break;
        }
        if (r) break;
        hold_t0 = GetTick();
        while (loaded < NCARDS) {
            if (!load_card(f, loaded, all + (long)loaded * CARD_BYTES)) {
                fclose(f); DisposeHandle(h); return -1;
            }
            loaded++;
        }
        if (GetTick() - hold_t0 < holds[ci])
            r = wait_ticks((unsigned)(holds[ci] - (GetTick() - hold_t0)));
        if (r) break;
        for (t = CARD_FADE; t >= 0; t--) {
            fade_cards(t, CARD_FADE);
            r = wait_ticks(1);
            if (r) break;
        }
        if (r) break;
        r = wait_ticks(14);   /* beat of black between cards */
    }
    fclose(f);
    DisposeHandle(h);
    fade_cards(0, CARD_FADE);
    clear_rows(0, 199);
    for (i = 0; i < 200L; i++) SCBDST[i] = SCB_PAL1;   /* back to 320 */
    if (r == 1) return 1;
    r = wait_ticks(12);   /* 200ms black, no SND.RISE yet */
    if (r == 1) return 1;
    return 0;
}

/* ---- CoGS card path ------------------------------------------------------
 * card_link: find the card, require the link up, and pull the first 32
 * bytes of FLYDATA.BIN with a Range GET. That one request does DNS, TCP and
 * the TLS handshake on the card, proves the URL is live, and hands back the
 * header (magic, count, first bodyId) for the block-id and count lines.
 * TLS_INFO afterwards gives the version and cipher for the Peer line.
 * Returns 1 on success; 0 means take the disk branch. */
static int card_link(void)
{
    cogs_status_reply st;
    cogs_http_reply hr;
    int rc;

    g_cslot = cogs_io_slot_find(&g_io, &g_slot);
    if (!g_cslot) return 0;
    cogs_init(&g_c, &g_io);
    if (cogs_get_status(&g_c, &st) != COGS_OK || st.link_state != 2) {
        g_cslot = 0;
        return 0;
    }
    g_probe_len = 0;
    rc = cogs_http_request(&g_c, 0, "GET", FLY_URL, "Range: bytes=0-31\r\n",
                           0, 0, COGS_HTTP_KEEPALIVE,
                           g_probe, sizeof(g_probe), &hr);
    if (rc != COGS_OK || hr.err != COGS_E_OK ||
        (hr.status != 206 && hr.status != 200) || hr.body_len < 6 ||
        g_probe[0] != 'F' || g_probe[1] != 'L' || g_probe[2] != 'Y' || g_probe[3] != '1') {
        g_cslot = 0;
        return 0;
    }
    g_probe_len = hr.body_len;
    memset(&g_tls, 0, sizeof(g_tls));
    cogs_tls_info(&g_c, &g_tls);
    return 1;
}

/* "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256" -> "ECDHE-RSA-AES-128-GCM-SHA256",
   "TLS1-3-AES-128-GCM-SHA256" -> "AES-128-GCM-SHA256", capped to fit. */
static void compact_cipher(char *out, const char *in, int cap)
{
    const char *p = in;
    int n = 0;
    if (strncmp(p, "TLS1-3-", 7) == 0) p += 7;
    else if (strncmp(p, "TLS-", 4) == 0) p += 4;
    while (*p && n < cap - 1) {
        if (strncmp(p, "-WITH", 5) == 0) { p += 5; continue; }
        out[n++] = *p++;
    }
    out[n] = 0;
}

static void tls_version(char *out, const char *v)
{
    /* "TLSv1.3" -> "TLS 1.3"; anything else passes through */
    if (strncmp(v, "TLSv", 4) == 0) sprintf(out, "TLS %s", v + 4);
    else if (*v) strcpy(out, v);
    else strcpy(out, "TLS");
}

/* Progress bar geometry shared by both branches. */
#define BAR_X0 (BOOT_X0 + 12 + 2)
#define BAR_W  (38 * 12 - 4)
static int g_barfill;
static void bar_begin(void)
{
    int row, x;
    dc640(&FMONO, BOOT_X0, g_cy, '[', 1, 1);
    dc640(&FMONO, BAR_X0 + BAR_W + 2, g_cy, ']', 1, 1);
    for (row = 1; row <= 7; row++)
        for (x = BAR_X0; x < BAR_X0 + BAR_W; x++) pix640(x, g_cy + row, 1);
    g_barfill = 0;
}

static void bar_update(unsigned long got, unsigned long total)
{
    char b[8];
    int nfx, row, x;
    unsigned pct;
    if (total == 0) total = 1;
    if (got > total) got = total;
    nfx = (int)((got * (unsigned long)BAR_W) / total);
    for (x = g_barfill; x < nfx; x++)
        for (row = 1; row <= 7; row++) pix640(BAR_X0 + x, g_cy + row, 3);
    if (nfx > g_barfill) g_barfill = nfx;
    pct = (unsigned)((got * 100UL) / total);
    sprintf(b, "%u%%", pct);
    g_cx = text640(&FMONO, BAR_X0 + BAR_W + 2 + 24, g_cy, b, 1, 1);
}

/* Stream FLYDATA.BIN over the card into dst, moving the bar per DATA frame.
   Same demux as cogslib's collector, with a 32-bit byte count (the file is
   past 64 KB) and a progress hook. Returns 1 on HTTP 200 + complete body. */
static int card_fetch(unsigned char *dst, unsigned long cap, unsigned long *got,
                      unsigned long expect)
{
    static cogs_u8 fr[2048];
    cogs_u16 n, i;
    cogs_u8 op;
    unsigned long frames, noprog = 0;
    int rc;

    *got = 0;
    if (cogs_http_begin(&g_c, 0, "GET", FLY_URL, 0, 0, 0, COGS_HTTP_KEEPALIVE) != COGS_OK)
        return 0;
    for (frames = 0; frames < 200000UL; frames++) {
        rc = cogs_recv_frame(&g_c, &op, fr, sizeof(fr), &n);
        if (rc != COGS_OK) return 0;
        if (op == COGS_EV_DATA) {
            if (n >= 1 && fr[0] == 0) {
                for (i = 1; i < n; i++)
                    if (*got < cap) dst[(*got)++] = fr[i];
                bar_update(*got, expect);
                noprog = 0;
                /* Esc during the transfer: stop pulling and quit cleanly */
                if ((KBD[0] & 0x80) && (KBD[0] & 0x7F) == 0x1B) {
                    g_escape = 1;
                    return 0;
                }
                continue;
            }
        } else if (op == COGS_EV_HTTP_DONE) {
            if (n >= 7 && fr[0] == 0) {
                unsigned status = (unsigned)fr[1] | ((unsigned)fr[2] << 8);
                return status == 200 && *got >= 6;
            }
        } else if (op == COGS_EV_ERROR) {
            return 0;
        }
        if (++noprog > 4096UL) return 0;
    }
    return 0;
}

/* Swap the parsed block for the bytes the card just delivered. */
static int adopt_block(Handle h, unsigned long len)
{
    Handle old = g_fh;
    unsigned char *oldp = g_file;
    unsigned long oldlen = g_flen;
    g_fh = h;
    g_file = (unsigned char *)*h;
    g_flen = len;
    if (parse_block()) {
        DisposeHandle(old);
        return 1;
    }
    g_fh = old; g_file = oldp; g_flen = oldlen;
    parse_block();
    return 0;
}

static int boot_seq(void)
{
    char buf[80], cip[40], ver[16];
    unsigned bid, count, i;
    unsigned long t0, now, expect;
    int r, viacard;
    long row;

    *NEWVIDEO |= 0x80;
    *BORDERREG &= 0xF0;
    shr_on_black();
    if (!blit_boot_shr()) {
        shr_on_black();
        for (row = BOOT_Y0; row < 200; row++) SCBDST[row] = SCB_TEXT;
        set_textpal(1, 1, 1);
    }
    g_bootdraw = 1;

    g_cx = BOOT_X0;
    g_cy = BOOT_Y0;

    r = type_str("CoGS TLS link ", COL_BODY);
    if (r) return r;
    /* slot scan + link check + TLS probe run while the leader fills */
    for (i = 0; i < 4; i++) {
        g_cx = dc640(&FMONO, g_cx, g_cy, '.', 0, 1);
        r = wait_ticks(1);
        if (r) return r;
    }
    viacard = card_link();
    for (; i < 20; i++) {
        g_cx = dc640(&FMONO, g_cx, g_cy, '.', 0, 1);
        r = wait_ticks(1);
        if (r) return r;
    }
    if (viacard) {
        r = type_str(" ESTABLISHED", COL_HEAD);
        if (r) return r;
        boot_nl();
        tls_version(ver, g_tls.version);
        compact_cipher(cip, g_tls.cipher, 26);
        sprintf(buf, "Peer: a2cogs.com  %s  %s", ver, cip);
        r = type_str(buf, COL_BODY);
        if (r) return r;
    } else {
        r = type_str(" NO CARD FOUND", COL_HEAD);
        if (r) return r;
        boot_nl();
        r = type_str("Loading cached neuron block from disk", COL_BODY);
        if (r) return r;
    }
    r = wait_ticks(HOLD_LINE);
    if (r) return r;
    boot_nl();
    boot_blank();
    r = type_str("Source: Fruit Fly Brain Map", COL_HEAD);
    if (r) return r;
    boot_nl();
    r = type_str("The complete wiring of a fruit fly's nervous", COL_BODY);
    if (r) return r;
    boot_nl();
    r = type_str("system: 166,700 neurons, 125 million connections.", COL_BODY);
    if (r) return r;
    boot_nl();
    r = type_str("Published Sept 3, 2026.", COL_BODY);
    if (r) return r;
    r = wait_ticks(HOLD_LINE);
    if (r) return r;
    boot_nl();
    boot_blank();

    /* Block id and count: from the card's probe bytes on the card path
       (same header the disk copy carries), else from the disk file. */
    if (viacard && g_probe_len >= 6) {
        unsigned long s = 0;
        for (i = 0; i < g_probe_len; i++) s += g_probe[i];
        bid = (unsigned)(s & 0xFFFF);
        count = rd16(g_probe + 4);
    } else {
        bid = file_blockid();
        count = g_ncount;
    }
    sprintf(buf, "Requesting neuron block 0x%04X ", bid);
    r = type_str(buf, COL_BODY);
    if (r) return r;
    for (i = 0; i < 6; i++) {
        g_cx = dc640(&FMONO, g_cx, g_cy, '.', 0, 1);
        r = wait_ticks(1);
        if (r) return r;
    }
    sprintf(buf, " %u neurons", count);
    r = type_str(buf, COL_HEAD);
    if (r) return r;
    boot_nl();
    expect = g_flen;   /* the cached copy is the same block, so its size is the transfer size */
    sprintf(buf, "Receiving %lu bytes", expect);
    r = type_str(buf, COL_BODY);
    if (r) return r;
    r = wait_ticks(HOLD_LINE);
    if (r) return r;
    boot_nl();
    boot_blank();

    bar_begin();
    if (viacard) {
        /* real transfer drives the bar */
        Handle h = NewHandle(200000L, userid(), attrLocked | attrFixed, 0L);
        unsigned long got = 0;
        int ok = 0;
        if (!toolerror()) {
            ok = card_fetch((unsigned char *)*h, 200000L, &got, expect);
            if (ok) ok = adopt_block(h, got);
            if (!ok) DisposeHandle(h);
            if (g_escape) return 1;
        }
        g_netbytes = got;
        bar_update(ok ? expect : got, expect);
        if (!ok) {
            /* card fetch fell over mid-way: finish on the cached block */
            bar_update(expect, expect);
        }
    } else {
        /* Disk read was instant; throttle the bar to ~1.5 s so it reads on video. */
        t0 = GetTick();
        for (;;) {
            now = GetTick();
            if (now - t0 > 90UL) now = t0 + 90UL;
            bar_update(((now - t0) * expect) / 90UL, expect);
            if (now - t0 >= 90UL) break;
            r = wait_ticks(1);
            if (r) return r;
        }
    }
    r = wait_ticks(20);   /* ~330ms after 100%, no SND.DONE yet */
    if (r) return r;
    boot_nl();
    boot_blank();
    r = type_str("Block received. Starting analysis on the 65C816.", COL_HEAD);
    if (r) return r;
    boot_nl();
    boot_blank();
    r = type_str("Apple IIgs  Sept 15, 1986 - Sept 15, 2026", COL_BODY);
    if (r) return r;
    r = wait_ticks(45);
    if (r == 1) return 1;

    /* Wipe: each row flips to the text palette (level 0 is black) and
       clears, so rows below the wipe line keep their own palettes. */
    g_bootdraw = 0;
    for (row = 0; row < 200; row++) {
        unsigned char *p = PIXDST + row * 160L;
        SCBDST[row] = SCB_TEXT;
        for (i = 0; i < 160; i++) p[i] = 0;
        if ((row & 7) == 7) {
            r = wait_ticks(1);
            if (r == 1) return 1;
        }
    }
    return 0;
}

/* 0 run the viewer, 1 Esc, -1 data load failed (caller reports) */
static int intro(void)
{
    int r;
    r = title_cards_640();
    if (r == 1) return 1;
    /* player wants a whole bank at $xx0000; grab it before the data block */
    music_load();
    /* the cached block loads here, behind the cards, not before them */
    if (!load_file()) return -1;
    if (!parse_block()) return -1;
    r = boot_seq();
    if (r == 1) return 1;
    return 0;
}

static void viewer(void)
{
    Neuron *nr;
    int ang = 64;   /* long axis into the screen plane on the first frame */
    int k;
    unsigned long now;

    g_cur = 0;
    g_N = 0;
    g_S = 0;
    g_auto = 1;
    g_fps = 0;
    g_nps = 0;
    g_frames = 0;
    g_nodesacc = 0;
    g_haveprev = 0;
    g_needtext = 1;
    g_chrome = 0;
    g_relabel = 1;
    g_bootdraw = 0;
    g_hue = 0;
    g_escape = 0;
    STROBE[0] = 0;   /* drop whatever key launched us or skipped the intro */
    choose_scale(&g_nr[0]);
    mark_leaves(&g_nr[0]);
    shr_on_black();
    install_ramp();
    draw_grid();
    music_start();
    g_t0 = g_nr0 = g_secmark = GetTick();

    for (;;) {
        nr = &g_nr[g_cur];
        /* chrome and labels first, so a fresh screen or a new neuron
           shows its frame and text before the wire starts drawing */
        if (g_needtext) {
            draw_text(nr, GetTick());
            g_needtext = 0;
        }
        draw_neuron(nr, ang);
        if (g_escape) break;
        ang = (ang + 3) & 255;   /* ~1.5 fps here; one turn in about a minute */
        g_frames++;
        g_nodesacc += nr->nNodes;
        now = GetTick();
        if (now - g_secmark >= 60UL) {
            g_fps = g_frames;
            g_nps = g_nodesacc;
            g_frames = 0;
            g_nodesacc = 0;
            g_secmark = now;
            g_needtext = 1;
        }
        if (g_auto && (now - g_nr0) >= DWELL) advance();
        k = getkey();
        if (k == 0x1B) break;
        if (k == ' ' || k == 0x0D) advance();
        if (k == 'A' || k == 'a') g_auto = !g_auto;
        if ((k == 'M' || k == 'm') && music_ok()) { music_mute(); draw_mute(); }
        if ((k == 'U' || k == 'u') && music_ok()) { music_unmute(); draw_mute(); }
    }
}

int main(void)
{
    int y;
    for (y = 0; y < 200; y++) g_rowp[y] = PIXDST + (long)y * 160L;
    fonts_init();
    g_sh = NewHandle(SHR_SAVE, userid(), attrLocked | attrFixed, 0L);
    if (toolerror()) { printf("NewHandle save %04x\n", toolerror()); return 1; }
    g_save = (unsigned char *)*g_sh;
    /* go black the moment we start, before the disk reads */
    save_desktop();
    shr_on_black();
    {
        int r = intro();
        if (r == -1) {
            restore_desktop();
            printf("GSFLY: could not load GSFLY.DATA\n");
            DisposeHandle(g_sh);
            return 1;
        }
        if (r == 0) viewer();
    }
    music_stop();
    restore_desktop();
    DisposeHandle(g_fh);
    DisposeHandle(g_sh);
    return 0;
}
