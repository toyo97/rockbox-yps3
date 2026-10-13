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
#ifndef SDRAM_TARGET_H
#define SDRAM_TARGET_H

#define SDRAM_BASE  0x08000000
#define SDRAM_SIZE  (32 * 1024 * 1024)

/* for large bootloader buffers that are only used after sdram_init() */
#ifdef BOOTLOADER
#define SDRAM_BSS_ATTR __attribute__((section(".sdram")))
#else
#define SDRAM_BSS_ATTR
#endif

void sdram_init(void);
int sdram_test(void);

#endif
