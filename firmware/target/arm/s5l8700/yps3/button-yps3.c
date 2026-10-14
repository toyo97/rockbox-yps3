/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 Bertrik Sikken
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

#include <stdbool.h>
#include "config.h"

#include "inttypes.h"
#include "s5l87xx.h"
#include "button.h"
#include "button-target.h"

/*  Button driver for the touch keys on the Samsung YP-S3

    The exact controller is not known, but it is likely from Melfas.
    
    The protocol is as follows:
    * the communication is done using three signals: DRDY, DCLK and DOUT
    * in the idle state these signals are all high.
    * when a key is touched or released, the key controller pulls down DRDY
      and outputs the first bit of a 20-bit word on its DOUT signal.
    * the CPU stores the bit, then acknowledges it by toggling the DCLK signal.
    * the key controller prepares the next bit, then toggles its DRDY output,
      unless all 20 bits have been transferred (in that case it stays high).
    * the 20-bit word contains separate bits for each button, some fixed bits
      and a bit indicating the number of keys pressed (modulo 2).
 */


void button_init_device(void)
{
    /* P0.5/P1.0 power switch input */
    PCON0 &= ~(3 << 10);    
    PCON1 &= ~0x0000000F;
    
    /* P1.3 headphones detect input */
    PCON1 &= ~0x0000F000;

    /* P1.5 DATA, P1.6 DRDY inputs (touch key controller) */
    PCON1 &= ~0x0FF00000; 
    
    /* P3.4 DCLK output (touch key controller) */
    PCON3 = (PCON3 & ~0x000F0000) | 0x00010000;
    PDAT3 |= (1 << 4);
    
    /* P4.3 hold switch input */
    PCON4 &= ~0x0000F000;
}

/*  Touch key words seen on the hardware: 0xA4001 with one bit per key
    (9 BACK, 8 UP, 7 MENU, 6 LEFT, 5 SELECT, 4 RIGHT, 3 DOWN) and bit 1 set
    when an odd number of keys is touched. One word per touch and release.
    Neighbouring pads often register together (UP with BACK or MENU,
    SELECT with a direction), which gives button combinations no keymap
    knows. Such words are resolved to one key:
    * a key that was already held alone stays the key while others join;
    * otherwise SELECT wins, then the directions (UP, DOWN, LEFT, RIGHT),
      then MENU and BACK: the d-pad is what gets pressed most and what
      fingers drift onto, BACK and MENU sit in the corners next to UP.
    POWER is a separate switch and combines with any touch key. */
#define TKEY_KEYS       0x3f8           /* bits 9..3 */
#define TKEY_PARITY     0x002
#define TKEY_FIXED      0xa4001         /* the other bits */

static const struct {
    int bit, button;
} tkey_map[] = {        /* in order of preference */
    { 1 << 5, BUTTON_SELECT },
    { 1 << 8, BUTTON_UP },
    { 1 << 3, BUTTON_DOWN },
    { 1 << 6, BUTTON_LEFT },
    { 1 << 4, BUTTON_RIGHT },
    { 1 << 7, BUTTON_MENU },
    { 1 << 9, BUTTON_BACK },
};
#define TKEY_NUM (int)(sizeof(tkey_map) / sizeof(tkey_map[0]))

/* for the debug screen: words received, rejected, resolved multi-touch
   words, and the last words, newest first */
unsigned long tkey_words, tkey_bad, tkey_multi;
int tkey_hist[8];
static int tkey_button;                 /* the resolved key */

/* waits for DRDY to reach 'level'; false after a few milliseconds (the
   controller's bit rate isn't known, it used to be waited for forever) */
static bool tkey_wait(unsigned int level)
{
    int timeout = 200000;
    while (((PDAT1 >> 6) & 1) != level) {
        if (--timeout == 0) {
            return false;
        }
    }
    return true;
}

/* reads a 20-bit word if the controller has one, -1 otherwise */
static int tkey_read(void)
{
    int value = 0;
    int i;

    /* check activity */
    if (PDAT1 & (1 << 6)) {
        return -1;
    }

    for (i = 0; i < 10; i++) {
        /* sample bit from falling edge of DRDY */
        if (!tkey_wait(0)) {
            break;
        }
        value <<= 1;
        if (PDAT1 & (1 << 5)) {
            value |= 1;
        }

        /* acknowledge on DCLK */
        PDAT3 &= ~(1 << 4);

        /* sample bit from rising edge of DRDY */
        if (!tkey_wait(1)) {
            PDAT3 |= (1 << 4);
            break;
        }
        value <<= 1;
        if (PDAT1 & (1 << 5)) {
            value |= 1;
        }

        /* acknowledge on DCLK */
        PDAT3 |= (1 << 4);
    }

    tkey_words++;
    for (i = 7; i > 0; i--) {
        tkey_hist[i] = tkey_hist[i - 1];
    }
    tkey_hist[0] = value;
    return value;
}

static bool tkey_valid(int word)
{
    if ((word & ~(TKEY_KEYS | TKEY_PARITY)) != TKEY_FIXED) {
        return false;
    }
    /* parity: bit 1 is set for an odd number of keys */
    int keys = word & TKEY_KEYS, odd = 0;
    while (keys) {
        odd ^= 1;
        keys &= keys - 1;
    }
    return odd == !!(word & TKEY_PARITY);
}

static void tkey_update(void)
{
    int word = tkey_read();
    int i, n = 0, button = 0;

    if (word < 0) {
        return;
    }
    if (!tkey_valid(word)) {
        tkey_bad++;             /* keep the previous state */
        return;
    }
    for (i = 0; i < TKEY_NUM; i++) {
        if (word & tkey_map[i].bit) {
            if (n++ == 0) {
                button = tkey_map[i].button;
            }
            if (tkey_map[i].button == tkey_button) {
                button = tkey_button;   /* already held: keep it */
            }
        }
    }
    if (n > 1) {
        tkey_multi++;
    }
    tkey_button = button;
}

int button_read_device(void)
{
    int buttons = 0;

    /* touch keys: read the words even on hold, to stay in step */
    tkey_update();

    /* hold switch */
    if (button_hold()) {
        return 0;
    }

    /* power button */
    if (PDAT1 & (1 << 0)) {
        buttons |= BUTTON_POWER;
    }
    return buttons | tkey_button;
}

bool button_hold(void)
{
    return (PDAT4 & (1 << 3));
}

bool headphones_inserted(void)
{
    return ((PDAT1 & (1 << 3)) == 0);
}

