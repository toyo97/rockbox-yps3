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
#include "audio.h"
#include "audiohw.h"
#include "s5l87xx.h"

/* P5.0 is the headphone amplifier mute (0 = muted) */
void audio_amp_enable(bool enable)
{
    PCON5 = (PCON5 & ~0x0000000F) | 0x00000001;
    if (enable) {
        PDAT5 |= 1;
    }
    else {
        PDAT5 &= ~1;
    }
}

void audio_set_output_source(int source)
{
    (void)source;
}

void audio_input_mux(int source, unsigned flags)
{
    (void)flags;

    /* the FM tuner is connected to the line inputs of the codec and is
       routed to the headphones in analog bypass */
    audiohw_set_bypass(source == AUDIO_SRC_FMRADIO);
}
