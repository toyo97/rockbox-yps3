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
#include "powermgmt.h"
#include "adc.h"

/* TODO: calibrate on real discharge curves, these are generic Li-Po */
unsigned short battery_level_disksafe = 3500;
unsigned short battery_level_shutoff = 3400;

unsigned short percent_to_volt_discharge[11] =
{
    3450, 3600, 3680, 3730, 3770, 3810, 3860, 3930, 4010, 4090, 4180
};

unsigned short percent_to_volt_charge[11] =
{
    3550, 3800, 3880, 3930, 3970, 4010, 4060, 4110, 4150, 4180, 4200
};

/* ADC channel 2 is the battery, full scale 1023 = about 4.65 V. According
   to the wiki the reading is only valid with the USB cable plugged in. */
int _battery_voltage(void)
{
    return (adc_read(2) * 4650) >> 10;
}
