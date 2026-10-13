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
#include "s5l87xx.h"

/*  The battery is on ADC channel 2. On battery power the reading tops out
    at 1023 when the battery is nearly full; the original firmware uses raw
    levels 898 (low) / 930 / 956 / 983 / 1013 / 1023 (full). With USB
    power the ADC reference is different: a full battery reads ~945.
    The millivolt scales below are estimates from these points (on
    battery ~3.95 mV per count, on USB ~4.4 mV per count). */
unsigned short battery_level_disksafe = 3500;
unsigned short battery_level_shutoff = 3450;

/* from the original firmware's levels, 0% = raw 898, 100% = raw 1023 */
unsigned short percent_to_volt_discharge[11] =
{
    3547, 3600, 3650, 3700, 3750, 3790, 3830, 3880, 3930, 3990, 4040
};

unsigned short percent_to_volt_charge[11] =
{
    3600, 3750, 3820, 3870, 3910, 3950, 3990, 4030, 4080, 4130, 4180
};

int _battery_voltage(void)
{
    int raw = adc_read(2);

    if (PDAT1 & (1 << 1))           /* USB power: other ADC reference */
        return raw * 4400 / 1000;
    return raw * 3950 / 1000;
}
