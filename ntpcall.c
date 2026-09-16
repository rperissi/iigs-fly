/* JSL trampoline for NinjaTrackerPlus. Compiled at -O0.
   The 5-byte slot lives in data (a real 24-bit pointer), not a
   function address. */
#pragma noroot
#pragma optimize -1

unsigned ntpA, ntpX, ntpAY, ntpR, ntpC;

/* 22 xx xx xx 6B  = JSL dest / RTL */
unsigned char ntpJ[5] = { 0x22, 0x00, 0x00, 0x00, 0x6B };

void ntpSetDest(unsigned long dest)
{
    ntpJ[1] = (unsigned char)dest;
    ntpJ[2] = (unsigned char)(dest >> 8);
    ntpJ[3] = (unsigned char)(dest >> 16);
}

/* Globals use long addressing (large model; DBR is not our bank).
   Carry comes back through an 8-bit pull so the stack stays even. */
asm void ntpCall(void)
{
    php
    rep #0x30
    lda >ntpX
    tax
    lda >ntpAY
    tay
    lda >ntpA
    jsl >ntpJ
    sta >ntpR
    php
    sep #0x20
    pla
    and #1
    sta >ntpC
    rep #0x20
    plp
    rtl
}
