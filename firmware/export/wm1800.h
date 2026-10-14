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
#ifndef _WM1800_H
#define _WM1800_H

/* Wolfson WM1800 as used in the Samsung YP-S3. There is no public
   documentation; its register map matches the WM8960. */

#define AUDIOHW_CAPS (LIN_GAIN_CAP)

AUDIOHW_SETTING(VOLUME,     "dB", 0, 1, -74,  6, -25)
#ifdef HAVE_RECORDING
/* ADC volume, -17..+30 dB in 0.5 dB steps (register 0xc3 = 0 dB) */
AUDIOHW_SETTING(LEFT_GAIN,  "dB", 1, 1, -34, 60, 0, val * 5)
AUDIOHW_SETTING(RIGHT_GAIN, "dB", 1, 1, -34, 60, 0, val * 5)
#endif

#define WM1800_LINVOL       0x00
#define WM1800_RINVOL       0x01
#define WM1800_LOUT1        0x02
#define WM1800_ROUT1        0x03
#define WM1800_OUT1_ZC      (1 << 7)
#define WM1800_OUT1_VU      (1 << 8)
#define WM1800_CLOCK1       0x04
#define WM1800_DACCTL1      0x05
#define WM1800_DACCTL1_DACMU (1 << 3)
#define WM1800_DACCTL2      0x06
#define WM1800_AINTF        0x07
#define WM1800_AINTF_I2S    (2 << 0)
#define WM1800_AINTF_16BIT  (0 << 2)
#define WM1800_AINTF_MS     (1 << 6)
#define WM1800_LDACVOL      0x0a
#define WM1800_RDACVOL      0x0b
#define WM1800_DACVOL_VU    (1 << 8)
#define WM1800_RESET        0x0f
#define WM1800_PWR1         0x19
#define WM1800_PWR1_VMID_50K (1 << 7)
#define WM1800_PWR1_VMID_250K (2 << 7)
#define WM1800_PWR1_VMID_5K (3 << 7)
#define WM1800_PWR1_VMID_MASK (3 << 7)
#define WM1800_PWR1_VREF    (1 << 6)
#define WM1800_PWR1_AINL    (1 << 5)
#define WM1800_PWR1_AINR    (1 << 4)
#define WM1800_PWR2         0x1a
#define WM1800_PWR2_DACL    (1 << 8)
#define WM1800_PWR2_DACR    (1 << 7)
#define WM1800_PWR2_LOUT1   (1 << 6)
#define WM1800_PWR2_ROUT1   (1 << 5)
#define WM1800_LOUTMIX      0x22
#define WM1800_ROUTMIX      0x25
#define WM1800_OUTMIX_D2O   (1 << 8)    /* DAC to output mixer */
#define WM1800_OUTMIX_I2O   (1 << 7)    /* LINPUT3/RINPUT3 to output mixer */
#define WM1800_OUTMIX_I2OVOL(x) ((x) << 4)  /* 0 = 0 dB .. 7 = -21 dB */
#define WM1800_PWR3         0x2f
#define WM1800_PWR3_LOMIX   (1 << 3)
#define WM1800_PWR3_ROMIX   (1 << 2)

#define WM1800_LADCVOL      0x15
#define WM1800_RADCVOL      0x16
#define WM1800_ADCVOL_VU    (1 << 8)
#define WM1800_ADCVOL_0DB   0xc3        /* 0.5 dB steps */
#define WM1800_LADCPATH     0x20
#define WM1800_RADCPATH     0x21
#define WM1800_LINBMIX      0x2b
#define WM1800_RINBMIX      0x2c
#define WM1800_INBMIX_IN3BOOST(x) ((x) << 4)  /* 0 = off .. 5 = 0 dB, 7 = +6 dB */
#define WM1800_LBYPASS      0x2d
#define WM1800_RBYPASS      0x2e
#define WM1800_BYPASS_B2O   (1 << 7)    /* input boost mixer to output mixer */
#define WM1800_PWR1_ADCL    (1 << 3)
#define WM1800_PWR1_ADCR    (1 << 2)

#define WM1800_NUM_REGISTERS 0x38

/* route the line input (the FM tuner) to the headphones */
void audiohw_set_bypass(bool enable);

#ifdef SAMSUNG_YPS3
/* headphone amplifier mute, in the target code */
void audio_amp_enable(bool enable);
#endif

#endif /* _WM1800_H */
