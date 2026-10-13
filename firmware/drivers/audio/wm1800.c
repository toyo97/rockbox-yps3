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

/*  Driver for the Wolfson WM1800 codec of the Samsung YP-S3.

    There is no public documentation for the WM1800, but its register map is
    that of the WM8960 (confirmed by the power management and mixer values
    that work on the YP-S3), so this follows the WM8960 data sheet.
    The codec is an I2S slave, the S5L8700 provides MCLK = 256 Fs.
    The headphones are on OUT1, the FM tuner on the line inputs.
*/

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "sound.h"
#include "wmcodec.h"
#include "audiohw.h"

static unsigned short regs[WM1800_NUM_REGISTERS];

static void wm1800_write(int reg, unsigned int val)
{
    regs[reg] = val;
    wmcodec_write(reg, val);
}

static void wm1800_set(int reg, unsigned int bits)
{
    wm1800_write(reg, regs[reg] | bits);
}

static void wm1800_clear(int reg, unsigned int bits)
{
    wm1800_write(reg, regs[reg] & ~bits);
}

/* tenths of dB (-740..60) to the OUT1 volume register */
static int vol_tenthdb2hw(int db)
{
    /* 0x7f = +6 dB, 0x79 = 0 dB, 0x30 = -73 dB, below that mute */
    if (db <= -740) {
        return 0;
    }
    return db / 10 + 73 + 0x30;
}

static void audiohw_mute(bool mute)
{
    if (mute) {
        wm1800_set(WM1800_DACCTL1, WM1800_DACCTL1_DACMU);
    }
    else {
        wm1800_clear(WM1800_DACCTL1, WM1800_DACCTL1_DACMU);
    }
}

void audiohw_preinit(void)
{
    wm1800_write(WM1800_RESET, 0);

    wm1800_write(WM1800_DACCTL1, WM1800_DACCTL1_DACMU);

    /* fast start-up with the 5k divider, then switch to 50k */
    wm1800_write(WM1800_PWR1, WM1800_PWR1_VMID_5K | WM1800_PWR1_VREF);
    sleep(HZ / 10);
    wm1800_write(WM1800_PWR1, WM1800_PWR1_VMID_50K | WM1800_PWR1_VREF |
                              WM1800_PWR1_AINL | WM1800_PWR1_AINR);

    wm1800_write(WM1800_AINTF, WM1800_AINTF_I2S | WM1800_AINTF_16BIT);
    wm1800_write(WM1800_CLOCK1, 0);     /* SYSCLK = MCLK, DAC at /256 */

    wm1800_write(WM1800_LDACVOL, 0xff | WM1800_DACVOL_VU);  /* 0 dB */
    wm1800_write(WM1800_RDACVOL, 0xff | WM1800_DACVOL_VU);

    wm1800_write(WM1800_LOUTMIX, WM1800_OUTMIX_D2O);
    wm1800_write(WM1800_ROUTMIX, WM1800_OUTMIX_D2O);

    wm1800_write(WM1800_LOUT1, WM1800_OUT1_ZC | 0x60);
    wm1800_write(WM1800_ROUT1, WM1800_OUT1_ZC | WM1800_OUT1_VU | 0x60);

    wm1800_write(WM1800_PWR2, WM1800_PWR2_DACL | WM1800_PWR2_DACR |
                              WM1800_PWR2_LOUT1 | WM1800_PWR2_ROUT1);
    wm1800_write(WM1800_PWR3, WM1800_PWR3_LOMIX | WM1800_PWR3_ROMIX);
}

void audiohw_postinit(void)
{
    audiohw_mute(false);
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    wm1800_write(WM1800_LOUT1, WM1800_OUT1_ZC | vol_tenthdb2hw(vol_l));
    wm1800_write(WM1800_ROUT1, WM1800_OUT1_ZC | WM1800_OUT1_VU |
                               vol_tenthdb2hw(vol_r));
}

void audiohw_set_bypass(bool enable)
{
    if (enable) {
        wm1800_set(WM1800_LOUTMIX, WM1800_OUTMIX_I2O);
        wm1800_set(WM1800_ROUTMIX, WM1800_OUTMIX_I2O);
    }
    else {
        wm1800_clear(WM1800_LOUTMIX, WM1800_OUTMIX_I2O);
        wm1800_clear(WM1800_ROUTMIX, WM1800_OUTMIX_I2O);
    }
}

void audiohw_close(void)
{
    audiohw_mute(true);
    wm1800_write(WM1800_PWR3, 0);
    wm1800_write(WM1800_PWR2, 0);
    wm1800_write(WM1800_PWR1, 0);
}

void audiohw_set_frequency(int fsel)
{
    /* the S5L8700 always provides MCLK = 256 Fs */
    (void)fsel;
}
