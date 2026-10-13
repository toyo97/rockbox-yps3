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
#include "s5l87xx.h"
#include "usb.h"

/* TODO: a USB device driver (the core is the S3C2443 style HS UDC, see
   bootloader/samsung_yps3_usb.c). For now only cable detection. */

void usb_init_device(void)
{
    /* P1.1 USB detect input */
    PCON1 &= ~0x000000F0;
}

void usb_enable(bool on)
{
    (void)on;
}

int usb_detect(void)
{
    /* no USB mode yet: never report a connection, so Rockbox keeps
       running while charging */
    return USB_EXTRACTED;
}
