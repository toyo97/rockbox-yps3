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

#ifndef _NAND_TARGET_H_
#define _NAND_TARGET_H_

#include "config.h"

void nand_ll_init(void);
unsigned int nand_ll_read_id(int bank);
int nand_ll_reset(int bank);
int nand_ll_read_raw(int bank, unsigned int page, unsigned int col,
                     unsigned char *buf, unsigned int len);

/* TODO later: create nand_ll_read/write/erase prototypes */

#endif /* _NAND_TARGET_H_ */

