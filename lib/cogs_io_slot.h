/*
 * cogslib slot transport backend - the REAL card, Apple II slot registers.
 *
 * Real hardware only (GS via ORCA/C, 8-bit via cc65): it reads and writes
 * absolute slot I/O addresses, so it does not compile under a host gcc (use
 * cogs_io_stub for that). Register base is $C080 + slot*$10 - bank $E0 on the
 * GS, a plain 16-bit address on an 8-bit Apple II (spec section 1).
 *
 * The card's slot must be set to "Your Card" in the GS Control Panel or
 * DEVSEL never reaches the card.
 */

#ifndef COGS_IO_SLOT_H
#define COGS_IO_SLOT_H

#include "cogs.h"

typedef struct {
    int slot;       /* 1..7 */
} cogs_slot;

/* Bind io to a known slot. */
void cogs_io_slot_init(cogs_io *io, cogs_slot *s, int slot);

/* Scan slots 1..7 for the CoGS signature (ID==$A2, VERSION!=0, STATUS!=$FF).
 * On success, leaves io bound to that slot and returns the slot number;
 * returns 0 if no card is found. */
int cogs_io_slot_find(cogs_io *io, cogs_slot *s);

#endif
