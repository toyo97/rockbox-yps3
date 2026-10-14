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

/* The FM tuner is connected to LINPUT3/RINPUT3 of the codec: listening
   goes through the analog bypass, recording through the ADCs (with the
   boost mixers to the headphones). */
void audio_input_mux(int source, unsigned flags)
{
#ifdef HAVE_RECORDING
    static bool last_recording = false;
    bool recording = (source == AUDIO_SRC_FMRADIO) &&
                     (flags & SRCF_RECORDING);

    if (recording != last_recording) {
        if (recording) {
            audiohw_set_monitor(false);
            audiohw_enable_recording(false);
        }
        else {
            audiohw_disable_recording();
        }
        last_recording = recording;
    }
    if (!recording) {
        audiohw_set_monitor(source == AUDIO_SRC_FMRADIO);
    }
#else
    (void)flags;
    audiohw_set_monitor(source == AUDIO_SRC_FMRADIO);
#endif
}
