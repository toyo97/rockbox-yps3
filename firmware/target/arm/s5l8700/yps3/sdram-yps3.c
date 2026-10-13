/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "s5l87xx.h"
#include "sdram-target.h"

/*  SDRAM controller set-up for the 32 MB K4M561633G of the Samsung YP-S3.

    The sequence and the values are those of the first stage loader of the
    original firmware (stored in NAND at 0x180000, runs from IRAM), which
    sets up the SDRAM before loading Samsung's bootloader to 0x09E00000.
    Unlike that loader, this keeps the clock settings and the IRAM remap
    (MIUCON bit 0) made by Rockbox's crt0.
*/

static void sdram_delay(void)
{
    volatile int i;
    for (i = 0; i < 100; i++);
}

void sdram_init(void)
{
    MIU_REG(0x34) = 0x10100000;
    MIUS01PARA = 0x00110811;
    MIUS23PARA = 0x00110811;
    MIUAREF = 0x0000071B;
    MIUSDPARA = 0x00736DE6;

    /* SDR mode */
    MIUORG = (MIUORG & ~0x20000) | 1;

    /* 0x500 is what the original firmware writes; keep the remap bit */
    MIUCON = 0x500 | (MIUCON & 1);
    SSTL2_PAD_ON = 2;
    MIUAREF = 0x0000171B;
    MIUMRS = 0x37;

    MIUCOM = 0x203;     /* precharge all banks */
    sdram_delay();
    MIUCOM = 0x303;     /* auto refresh */
    sdram_delay();
    MIUCOM = 0x303;
    sdram_delay();
    MIUCOM = 0x303;
    sdram_delay();
    MIUCOM = 0x103;     /* mode register set */
    sdram_delay();
    MIU_REG(0x48) = 0;
}

/* Returns the number of MB that work, checking for address aliasing too.
   Destroys the SDRAM contents. */
int sdram_test(void)
{
    volatile unsigned int *mem = (volatile unsigned int *)SDRAM_BASE;
    unsigned int words = SDRAM_SIZE / 4;
    unsigned int i, mb;

    /* write a unique value at the start of each MB first, to see aliasing */
    for (mb = 0; mb < SDRAM_SIZE >> 20; mb++) {
        mem[mb << 18] = 0xA5000000 | mb;
    }
    for (mb = 0; mb < SDRAM_SIZE >> 20; mb++) {
        if (mem[mb << 18] != (0xA5000000 | mb)) {
            return mb;
        }
    }

    for (i = 0; i < words; i++) {
        mem[i] = i ^ 0x5A5AA5A5;
    }
    for (i = 0; i < words; i++) {
        if (mem[i] != (i ^ 0x5A5AA5A5)) {
            return i >> 18;
        }
    }
    for (i = 0; i < words; i++) {
        mem[i] = ~i;
    }
    for (i = 0; i < words; i++) {
        if (mem[i] != ~i) {
            return i >> 18;
        }
    }
    return SDRAM_SIZE >> 20;
}
