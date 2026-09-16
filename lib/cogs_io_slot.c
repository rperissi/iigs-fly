/*
 * cogslib slot transport backend (real card, Apple II slot registers).
 * See cogs_io_slot.h.
 *
 * Two hosts, one register model ($C080 + slot*$10, spec section 1):
 *   - IIgs (ORCA/C): the slot I/O lives in bank $E0; a 32-bit long cast to a
 *     far volatile pointer is the working pattern from gs/cogstest.c.
 *   - 8-bit Apple II (cc65, __CC65__): a flat 16-bit address space, so the
 *     same softswitches are reached at plain $C080 with a 16-bit pointer.
 * Register reads must be side-effect free on the card (spec section 1), which
 * they are: every read here is a single load.
 */

#include "cogs_io_slot.h"

typedef volatile unsigned char *cogs_regptr;

static cogs_regptr slot_reg(int slot, int off)
{
#ifdef __CC65__
    return (cogs_regptr)(0xC080U + (unsigned)slot * 0x10U + (unsigned)off);
#else
    return (cogs_regptr)(0xE0C080L + (long)slot * 0x10L + (long)off);
#endif
}

static int slot_rd(cogs_io *io, int reg)
{
    cogs_slot *s;

    s = (cogs_slot *)io->priv;
    return *slot_reg(s->slot, reg);
}

static void slot_wr(cogs_io *io, int reg, cogs_u8 v)
{
    cogs_slot *s;

    s = (cogs_slot *)io->priv;
    *slot_reg(s->slot, reg) = v;
}

/* Burst pop: n DATA reads with the register address computed ONCE. The
 * per-byte rd() path recomputes the 32-bit slot address (a runtime multiply
 * under ORCA/C -O0) and pays a function call per byte, which is what made
 * multi-KB payloads (CDA catalog pages) crawl. Caller guarantees n bytes are
 * queued (RXLO/RXHI), so no per-byte status poll is needed.
 *
 * On the IIgs the drain is hand-unrolled 65816: the -O0 C byte loop costs
 * ~11 ms/KB, and the CDA's background heartbeat tick drains the FIFO INSIDE
 * the VBL interrupt - at that price the tick plus Tool225's DOC service
 * overran the 16.7 ms frame, VBL interrupts were lost, and the machine spent
 * its life in interrupt context (bg freeze investigation, 2026-08-22). The
 * unrolled loop (8-bit A, [dp] long-indirect load from the data register,
 * [dp],y store) is ~5 us/byte, roughly half the C loop, and it keeps the
 * whole interrupt chain inside one frame. */
static void slot_rdn(cogs_io *io, cogs_u8 *dst, cogs_u16 n)
{
    cogs_slot   *s = (cogs_slot *)io->priv;
    cogs_regptr  p = slot_reg(s->slot, 0 /* COGS_REG_DATA */);

#ifdef __CC65__
    while (n != 0) {
        *dst++ = *p;
        n--;
    }
#else
    unsigned long pl = (unsigned long)p;
    unsigned      off = 0;

    while ((unsigned)(n - off) >= 16) {
        asm {
            ldy off
            sep #32
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            lda [pl]
            sta [dst],y
            iny
            rep #32
            sty off
        }
    }
    dst += off;
    n = (cogs_u16)(n - off);
    while (n != 0) {
        *dst++ = *p;
        n--;
    }
#endif
}

void cogs_io_slot_init(cogs_io *io, cogs_slot *s, int slot)
{
    s->slot = slot;
    io->rd = slot_rd;
    io->wr = slot_wr;
    io->rdn = slot_rdn;
    io->priv = (void *)s;
}

int cogs_io_slot_find(cogs_io *io, cogs_slot *s)
{
    int     slot;
    cogs_u8 id, ver, st;

    for (slot = 1; slot <= 7; slot++) {
        id  = *slot_reg(slot, COGS_REG_ID);
        ver = *slot_reg(slot, COGS_REG_VERSION);
        st  = *slot_reg(slot, COGS_REG_STATUS);
        if (id == COGS_ID_BYTE && ver != 0 && st != 0xFF) {
            cogs_io_slot_init(io, s, slot);
            return slot;
        }
    }
    return 0;
}
