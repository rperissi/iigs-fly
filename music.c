/* NinjaTrackerPlus playback. Song starts with the neuron viewer.
   Volume is a player-side scale (not the system Sound CDev). */
#pragma noroot

#include <stdio.h>
#include <orca.h>
#include <Memory.h>
#include <sound.h>
#include "ntpcall.h"
#include "music.h"

#define NTP_PREPARE  0
#define NTP_PLAY     3
#define NTP_STOP     6
#define NTP_VOLUME   21
#define MUSIC_VOL    115
#define PLAYER_BYTES 0x8800L

static Handle g_plh, g_sngh;
static unsigned char *g_player;
static unsigned char *g_song;
static int g_ready;
static int g_on;
static int g_muted;
static int g_sndown;

static FILE *open_named(const char *name)
{
    static const char *pre[] = {
        "1/", "", "/GSFLY/GSFLY/", "/GSFLY800/", "/GSFLY/", 0
    };
    char path[40];
    FILE *f;
    int i;
    for (i = 0; pre[i]; i++) {
        sprintf(path, "%s%s", pre[i], name);
        f = fopen(path, "rb");
        if (f) return f;
    }
    return 0;
}

static unsigned long read_all(FILE *f, unsigned char *dst, unsigned long cap)
{
    unsigned long n = 0;
    size_t got;
    while (n + 512UL <= cap) {
        got = fread(dst + n, 1, 512, f);
        if (!got) break;
        n += got;
        if (got < 512) break;
    }
    return n;
}

static int ntp_jsl(unsigned off, unsigned a, unsigned x, unsigned y)
{
    ntpSetDest((unsigned long)g_player + off);
    ntpA = a;
    ntpX = x;
    ntpAY = y;
    ntpC = 0;
    ntpCall();
    return ntpC & 1;
}

static void sound_take(void)
{
    if (SoundToolStatus()) FFStopSound(0x7FFF);
    SoundShutDown();
    g_sndown = 1;
}

static void sound_give(void)
{
    if (!g_sndown) return;
    SoundStartUp((Word)userid());
    g_sndown = 0;
}

/* Player JMPs are intra-bank (org $xx0000). Need a bank start, not
   any attrBank fit. Try a whole free bank, then $0F0000 downward. */
static int grab_player(void)
{
    static unsigned long bank[14] = {
        0x0F0000L, 0x0E0000L, 0x0D0000L, 0x0C0000L,
        0x0B0000L, 0x0A0000L, 0x090000L, 0x080000L,
        0x070000L, 0x060000L, 0x050000L, 0x040000L,
        0x030000L, 0x020000L
    };
    int i;

    CompactMem();
    g_plh = NewHandle(65536L, userid(),
                      attrLocked | attrFixed | attrBank | attrNoCross, 0L);
    if (!toolerror()) {
        g_player = (unsigned char *)*g_plh;
        if (((unsigned long)g_player & 0xFFFFUL) == 0) return 1;
        DisposeHandle(g_plh);
        g_plh = 0;
    }
    for (i = 0; i < 14; i++) {
        g_plh = NewHandle(PLAYER_BYTES, userid(),
                          attrLocked | attrFixed | attrAddr | attrNoCross,
                          (Pointer)bank[i]);
        if (!toolerror()) {
            g_player = (unsigned char *)*g_plh;
            return 1;
        }
    }
    g_plh = 0;
    return 0;
}

int music_ok(void) { return g_ready; }
int music_muted(void) { return g_muted; }

int music_load(void)
{
    FILE *f;
    unsigned long n;

    g_ready = 0;
    g_on = 0;
    g_muted = 0;

    if (!grab_player()) return 0;
    f = open_named("NTPPLAYER");
    if (!f) { DisposeHandle(g_plh); g_plh = 0; return 0; }
    n = read_all(f, g_player, 65536UL);
    fclose(f);
    if (n < 32 || g_player[0] != 0x4C) {
        DisposeHandle(g_plh); g_plh = 0; return 0;
    }

    g_sngh = NewHandle(120000L, userid(), attrLocked | attrFixed, 0L);
    if (toolerror()) { DisposeHandle(g_plh); g_plh = 0; return 0; }
    g_song = (unsigned char *)*g_sngh;
    f = open_named("GSFLY.NTP");
    if (!f) {
        DisposeHandle(g_sngh); DisposeHandle(g_plh);
        g_sngh = 0; g_plh = 0; return 0;
    }
    n = read_all(f, g_song, 120000UL);
    fclose(f);
    if (n < 8 || g_song[0] != 'n' || g_song[1] != 'f'
        || g_song[2] != 'c' || g_song[3] != '!') {
        DisposeHandle(g_sngh); DisposeHandle(g_plh);
        g_sngh = 0; g_plh = 0; return 0;
    }
    g_ready = 1;
    return 1;
}

void music_start(void)
{
    unsigned long addr;
    if (!g_ready || g_on) return;
    sound_take();
    addr = (unsigned long)g_song;
    if (ntp_jsl(NTP_PREPARE, 0,
                (unsigned)(addr & 0xFFFFUL),
                (unsigned)((addr >> 16) & 0xFFFFUL))) {
        sound_give();
        return;
    }
    ntp_jsl(NTP_VOLUME, MUSIC_VOL, 0, 0);
    ntp_jsl(NTP_PLAY, 0, 0, 0);
    g_on = 1;
    g_muted = 0;
}

void music_stop(void)
{
    if (g_ready && g_on) ntp_jsl(NTP_STOP, 0, 0, 0);
    sound_give();
    g_on = 0;
    g_ready = 0;
    if (g_sngh) { DisposeHandle(g_sngh); g_sngh = 0; }
    if (g_plh) { DisposeHandle(g_plh); g_plh = 0; }
}

void music_mute(void)
{
    if (!g_on || g_muted) return;
    ntp_jsl(NTP_VOLUME, 0, 0, 0);
    g_muted = 1;
}

void music_unmute(void)
{
    if (!g_on || !g_muted) return;
    ntp_jsl(NTP_VOLUME, MUSIC_VOL, 0, 0);
    g_muted = 0;
}
