/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bertrik Sikken
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

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"

#include "inttypes.h"
#include "cpu.h"
#include "system.h"
#include "lcd.h"
#include "../kernel-internal.h"
#include "backlight.h"
#include "backlight-target.h"
#include "button.h"
#include "panic.h"
#include "power.h"
#include "common.h"
#include "usb.h"
#include "bitmaps/rockboxlogo.h"

#include "adc.h"
#include "adc-target.h"

#include "timer.h"

#include "i2c-s5l8700.h"
#include "dma-target.h"
#include "pcm.h"
#include "audiohw.h"
#include "rtc.h"
#include "tuner.h"
#include "si4700.h"
#include "fmradio_i2c.h"
#include "wmcodec.h"
#include "nand-target.h"

#define LONG_DELAY  200000
#define SHORT_DELAY  50000
#define PAUSE_DELAY  50000

static inline void delay(unsigned int duration)
{
    volatile unsigned int i;
    for(i=0;i<duration;i++);
}

// forward declaration
static int rds_decode(int line, struct si4700_dbg_info *nfo);
int lcd_yps3_type(void);

/*  NAND probe

    The main open question for this port is how the original firmware lays
    out its data in the NAND flash. The iPod Nano 2G (same SoC family) uses
    Samsung's "Whimory" FTL, which Rockbox already implements in
    target/arm/s5l8700/ipodnano2g/ftl-nano2g.c. This screen lets a tester
    look at raw pages and gathers some statistics that tell us whether the
    YP-S3 uses the same on-flash format:
    - nano2g keeps "DEVICEINFOSIGN" pages in the last 10% of each chip
    - byte 9 of the spare area of each page is a page type:
      0x40/0x41 user data, 0x43 FTL context, 0x44 block map,
      0x46 erase counters, 0x47 mount mark, 0x80 VFL context
*/

#define NAND_MAX_PAGE   4096
#define NAND_MAX_SPARE  128

static unsigned char page_buf[NAND_MAX_PAGE + NAND_MAX_SPARE];

struct nand_geometry {
    unsigned int pagesize;      /* main area bytes */
    unsigned int sparesize;     /* spare area bytes */
    unsigned int pagesperblock;
    unsigned int blocks;
};

/* derive geometry from the 4-byte ID (first byte in the low bits) */
static void nand_decode_id(unsigned int id, struct nand_geometry *geo)
{
    unsigned int devcode = (id >> 8) & 0xFF;
    unsigned int b4 = (id >> 24) & 0xFF;
    unsigned int blocksize;
    unsigned int megabits;

    geo->pagesize = 1024 << (b4 & 3);
    if (geo->pagesize > NAND_MAX_PAGE) {
        geo->pagesize = NAND_MAX_PAGE;
    }
    geo->sparesize = (geo->pagesize / 512) * (8 << ((b4 >> 2) & 1));
    if (geo->sparesize > NAND_MAX_SPARE) {
        geo->sparesize = NAND_MAX_SPARE;
    }
    blocksize = 65536 << ((b4 >> 4) & 3);
    geo->pagesperblock = blocksize / geo->pagesize;

    switch (devcode) {
    case 0xF1: megabits = 1024; break;
    case 0xDA: megabits = 2048; break;
    case 0xDC: megabits = 4096; break;
    case 0xD3: megabits = 8192; break;
    case 0xD5: megabits = 16384; break;
    case 0xD7: megabits = 32768; break;
    default:   megabits = 0; break;
    }
    geo->blocks = megabits ? (megabits / 8) * (1024 / (blocksize / 1024)) : 0;
}

static bool nand_bank_present(unsigned int id)
{
    return (id != 0) && (id != 0xFFFFFFFF);
}

static unsigned int button_wait_press(void)
{
    unsigned int button;

    /* wait for release of previous key, then for a new press */
    while (button_read_device() & BUTTON_MAIN);
    do {
        button = button_read_device() & BUTTON_MAIN;
    } while (button == 0);
    return button;
}

static void show_hex(int line, const char *label, const unsigned char *p,
                     unsigned int len)
{
    char str[64];
    unsigned int i, j;
    int col;

    lcd_puts(0, line++, label);
    for (i = 0; i < len; i += 8) {
        col = snprintf(str, sizeof(str), "%02X:", i & 0xFF);
        for (j = 0; j < 8; j++) {
            col += snprintf(str + col, sizeof(str) - col, "%02X", p[i + j]);
        }
        col += snprintf(str + col, sizeof(str) - col, " ");
        for (j = 0; j < 8; j++) {
            unsigned char c = p[i + j];
            str[col++] = (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        str[col] = '\0';
        lcd_puts(0, line++, str);
    }
}

/* Scan the chip and gather FTL hints, results are shown on screen */
static void nand_scan(int bank, const struct nand_geometry *geo)
{
    static unsigned int typecount[256];
    char str[64];
    unsigned int block, page, i, pagenum;
    unsigned int devinfo_hits = 0, devinfo_first = 0, errors = 0;
    unsigned int lowest;
    unsigned char spare[16];
    int line = 0;

    lcd_clear_display();
    lcd_puts(0, line++, "Scanning, please wait...");
    lcd_update();

    memset(typecount, 0, sizeof(typecount));

    /* spare type byte of the first page of every block */
    for (block = 0; block < geo->blocks; block++) {
        pagenum = block * geo->pagesperblock;
        if (nand_ll_read_raw(bank, pagenum, geo->pagesize, spare, 12)) {
            errors++;
            continue;
        }
        typecount[spare[9]]++;
        if ((block & 0xFF) == 0) {
            snprintf(str, sizeof(str), "block %u / %u", block, geo->blocks);
            lcd_puts(0, 1, str);
            lcd_update();
        }
    }

    /* "DEVICEINFOSIGN" pages, like the nano2g FTL looks for them */
    lowest = geo->blocks - geo->blocks / 10;
    for (block = geo->blocks - 1; block >= lowest && block > 0; block--) {
        for (page = geo->pagesperblock - 8; page < geo->pagesperblock; page++) {
            pagenum = block * geo->pagesperblock + page;
            if (nand_ll_read_raw(bank, pagenum, 0, page_buf, 16)) {
                errors++;
                continue;
            }
            if (memcmp(page_buf, "DEVICEINFOSIGN\0", 15) == 0) {
                if (devinfo_hits == 0) {
                    devinfo_first = pagenum;
                }
                devinfo_hits++;
            }
        }
    }

    lcd_clear_display();
    line = 0;
    snprintf(str, sizeof(str), "Scan of bank %d, %u blocks", bank, geo->blocks);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), "read errors: %u", errors);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), "DEVICEINFOSIGN: %u", devinfo_hits);
    lcd_puts(0, line++, str);
    if (devinfo_hits) {
        snprintf(str, sizeof(str), " first at page %u", devinfo_first);
        lcd_puts(0, line++, str);
    }
    lcd_puts(0, line++, "spare[9] of 1st page/block:");
    snprintf(str, sizeof(str), " FF(empty):%u", typecount[0xFF]);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), " 40:%u 41:%u", typecount[0x40], typecount[0x41]);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), " 43:%u 44:%u 46:%u", typecount[0x43],
             typecount[0x44], typecount[0x46]);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), " 47:%u 80:%u", typecount[0x47], typecount[0x80]);
    lcd_puts(0, line++, str);
    lcd_puts(0, line++, " other values (top 6):");
    /* show the most frequent remaining values */
    for (i = 0; i < 6; i++) {
        unsigned int v, best = 0, bestcount = 0;
        for (v = 0; v < 256; v++) {
            if (v == 0xFF || v == 0x40 || v == 0x41 || v == 0x43 ||
                v == 0x44 || v == 0x46 || v == 0x47 || v == 0x80) {
                continue;
            }
            if (typecount[v] > bestcount) {
                best = v;
                bestcount = typecount[v];
            }
        }
        if (bestcount == 0) {
            break;
        }
        snprintf(str, sizeof(str), "  %02X: %u", best, bestcount);
        lcd_puts(0, line++, str);
        typecount[best] = 0;
    }
    lcd_puts(0, line + 1, "Please photograph this screen");
    lcd_puts(0, line + 2, "Press any key");
    lcd_update();
    button_wait_press();
}

static void nand_probe(const unsigned int *ids)
{
    struct nand_geometry geo;
    char str[64];
    int bank = 0;
    unsigned int page = 0;
    unsigned int button;
    int rc, line;

    for (bank = 0; bank < 4 && !nand_bank_present(ids[bank]); bank++);
    if (bank == 4) {
        lcd_clear_display();
        lcd_puts(0, 0, "No NAND chips found");
        lcd_puts(0, 1, "Press any key");
        lcd_update();
        button_wait_press();
        return;
    }

    while (true) {
        nand_decode_id(ids[bank], &geo);
        if (geo.blocks && page >= geo.blocks * geo.pagesperblock) {
            page = 0;
        }

        rc = nand_ll_read_raw(bank, page, 0, page_buf,
                              geo.pagesize + geo.sparesize);

        lcd_clear_display();
        line = 0;
        snprintf(str, sizeof(str), "NAND bank %d ID %08X", bank, ids[bank]);
        lcd_puts(0, line++, str);
        snprintf(str, sizeof(str), "%u+%u %upg/blk %ublk",
                 geo.pagesize, geo.sparesize, geo.pagesperblock, geo.blocks);
        lcd_puts(0, line++, str);
        snprintf(str, sizeof(str), "pg %u blk %u.%u rc %d", page,
                 page / geo.pagesperblock, page % geo.pagesperblock, rc);
        lcd_puts(0, line++, str);
        show_hex(line, "data[0..63]:", page_buf, 64);
        line += 9;
        show_hex(line, "spare[0..63]:", page_buf + geo.pagesize, 64);
        line += 9;
        lcd_puts(0, line++, "U/D pg L/R blk MENU bank");
        lcd_puts(0, line++, "BACK scan  SELECT exit");
        lcd_update();

        button = button_wait_press();
        if (button & BUTTON_SELECT) {
            break;
        }
        else if (button & BUTTON_UP) {
            page++;
        }
        else if (button & BUTTON_DOWN) {
            if (page > 0) page--;
        }
        else if (button & BUTTON_RIGHT) {
            page += geo.pagesperblock;
        }
        else if (button & BUTTON_LEFT) {
            page = (page >= geo.pagesperblock) ? page - geo.pagesperblock : 0;
        }
        else if (button & BUTTON_MENU) {
            do {
                bank = (bank + 1) % 4;
            } while (!nand_bank_present(ids[bank]));
            page = 0;
        }
        else if (button & BUTTON_BACK) {
            if (geo.blocks) {
                nand_scan(bank, &geo);
            }
        }
    }

    while (button_read_device() & BUTTON_MAIN);
    lcd_clear_display();
}

void main(void)
{
    char mystring[64];
    int line, col;
    struct tm dt;
    int i;
    struct si4700_dbg_info si4700_info;
    int brightness = DEFAULT_BRIGHTNESS_SETTING;
    unsigned int button;
    unsigned int fm_frequency = 100700000;
    int audiovol = 0x60;
    unsigned nand_ids[4];
    
    // enable all peripherals
    PWRCON = 0;
    
    // disable all interrupts
    INTMSK = 0;
    
    // start with all GPIOs as input
    PCON0 = 0;
    PCON1 = 0;
    PCON2 = 0;
    PCON3 = 0;
    PCON4 = 0;
    PCON5 = 0;
    PCON6 = 0;
    PCON7 = 0;
    
    system_init();
    kernel_init();
    
    asm volatile("msr cpsr_c, #0x13\n\t"); // enable interrupts

    lcd_init();
    
    lcd_clear_display();
    lcd_bitmap(rockboxlogo, 0, 160, BMPWIDTH_rockboxlogo, BMPHEIGHT_rockboxlogo);
    lcd_update();

    power_init();

    /* Blink the LED behind the middle button once for LCD type 1 and twice
       for type 2. Only type 2 has been tested, so if the screen stays blank
       this tells us which display driver needs work. */
    PCON4 = (PCON4 & ~0x00000F00) | 0x00000100;   /* P4.2 output */
    for (i = 0; i < lcd_yps3_type(); i++) {
        PDAT4 |= (1 << 2);
        delay(LONG_DELAY * 60);
        PDAT4 &= ~(1 << 2);
        delay(LONG_DELAY * 60);
    }
    
    i2c_init();
    fmradio_i2c_init();
    adc_init();
    backlight_hw_init();
    button_init_device();
        
    // FM power
    si4700_init();
    tuner_power(true);
    si4700_set(RADIO_SLEEP, 0);
    si4700_set(RADIO_MUTE, 0);
    si4700_set(RADIO_REGION, 0);
    si4700_set(RADIO_FREQUENCY, fm_frequency);
    
    lcd_puts(0, 0, "Rockbox YP-S3 hardware test");

    // WM1800 codec configuration
    wmcodec_write(0x0F, 0);     // codec reset
    wmcodec_write(0x19, 0xF0);  // pwr mgmt1: VMID = 1, VREF, AINL, AINR
    wmcodec_write(0x1A, 0x60);  // pwr mgmt2: LOUT1, ROUT1
    wmcodec_write(0x2F, 0x0C);  // pwr mgmt3: LOMIX, ROMIX
    wmcodec_write(0x02, audiovol);              // LOUT1VOL
    wmcodec_write(0x03, audiovol | (1 << 8));   // ROUT1VOL
    wmcodec_write(0x22, (1 << 7) | (7 << 4));   // left out mix (1)
    wmcodec_write(0x25, (1 << 7) | (7 << 4));   // right out mix (2)
    
    // enable audio
    PCON5 = (PCON5 & ~0x0000000F) | 0x00000001;
    PDAT5 |= 1;

    nand_ll_init();
    for (i = 0; i < 4; i++) {
        nand_ll_reset(i);
        nand_ids[i] = nand_ll_read_id(i);
    }

    while (true)
    {
        line = 1;

#if 1   /* enable to see GPIOs */
        snprintf(mystring, 64, "%02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned)PDAT0, (unsigned)PDAT1, (unsigned)PDAT2, (unsigned)PDAT3,
            (unsigned)PDAT4, (unsigned)PDAT5, (unsigned)PDAT6, (unsigned)PDAT7);
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* enable this to see info about the RTC */
        rtc_read_datetime(&dt);
        snprintf(mystring, 64, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
            dt.tm_year + 1900, dt.tm_mon+1, dt.tm_mday,
            dt.tm_hour, dt.tm_min, dt.tm_sec);
        lcd_puts(0, line++, mystring);
#endif 

#if 1   /* enable this to see radio debug info */
        button = button_read_device();
        if (button & BUTTON_RIGHT) {
            fm_frequency += 100000;
            si4700_set(RADIO_FREQUENCY, fm_frequency);
        }
        if (button & BUTTON_LEFT) {
            fm_frequency -= 100000;
            si4700_set(RADIO_FREQUENCY, fm_frequency);
        }
        snprintf(mystring, 64, "FM frequency: %9d", fm_frequency);
        lcd_puts(0, line++, mystring);

        si4700_dbg_info(&si4700_info);
        col = snprintf(mystring, 64, "FM: ");
        for (i = 0; i < 16; i++) {
            col += snprintf(mystring + col, 64, "%04X ", si4700_info.regs[i]);
            if (((i + 1) % 4) == 0) {
                lcd_puts(0, line++, mystring);
                col = 4;
            }
        }
        line = rds_decode(line, &si4700_info);
#endif

#if 1   /* volume control with up/down keys */
        button = button_read_device();
        if (button & BUTTON_UP) {
            if (audiovol < 127) {
                audiovol++;
                wmcodec_write(0x02, audiovol);
                wmcodec_write(0x03, (1 << 8) | audiovol);
            }
        }
        if (button & BUTTON_DOWN) {
            if (audiovol > 0) {
                audiovol--;
                wmcodec_write(0x02, audiovol);
                wmcodec_write(0x03, (1 << 8) | audiovol);
            }
        }
        snprintf(mystring, 64, "volume %3d", audiovol);
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* enable this to see ADC info */
        snprintf(mystring, 64, "ADC: %04X %04X %04X %04X", 
            adc_read(0), adc_read(1), adc_read(2), adc_read(3));
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "ADC:USB %4d mV BAT %4d mV",
            (adc_read(0) * 6000) >> 10, (adc_read(2) * 4650) >> 10);
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* enable this so see USB info */
        snprintf(mystring, 64, "CLK %08X CLK2 %08X", (unsigned)CLKCON, (unsigned)CLKCON2);
        lcd_puts(0, line++, mystring);

        snprintf(mystring, 64, "%04X %04X %04X %04X", (unsigned)PHYCTRL, (unsigned)PHYPWR,
                 (unsigned)URSTCON, (unsigned)UCLKCON);
        lcd_puts(0, line++, mystring);

        snprintf(mystring, 64, "SCR %04X SSR %04X EIR %04X", (unsigned)USB_SCR,
                 (unsigned)USB_SSR, (unsigned)USB_EIR);
        lcd_puts(0, line++, mystring);

        snprintf(mystring, 64, "FAR %04X FNR %04X EP0 %04X", (unsigned)USB_FAR,
                 (unsigned)USB_FNR, (unsigned)USB_EP0SR);
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* button lights controlled by keypad */
        button = button_read_device();
        if (button & (BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT)) {
            PDAT3 |= (1 << 3);
        }
        else {
            PDAT3 &= ~(1 << 3); 
        }
        if (button & (BUTTON_BACK | BUTTON_MENU)) {
            PDAT3 |= (1 << 2);
        }
        else {
            PDAT3 &= ~(1 << 2);
        }
        if (button & (BUTTON_SELECT)) {
            PDAT4 |= (1 << 2);
        }
        else {
            PDAT4 &= ~(1 << 2);
        }
#endif

#if 1   /* backlight brightness controlled by up/down keys */
        button = button_read_device();
        if (button & BUTTON_MENU) {
            if (brightness < MAX_BRIGHTNESS_SETTING) {
                brightness++;
                backlight_hw_brightness(brightness);
            }
        }
        else  if (button & BUTTON_BACK) {
            if (brightness > MIN_BRIGHTNESS_SETTING) {
                brightness--;
                backlight_hw_brightness(brightness);
            }
        }
        snprintf(mystring, 64, "brightness %3d", brightness);
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* SELECT opens the NAND probe */
        button = button_read_device();
        if (button & BUTTON_SELECT) {
            nand_probe(nand_ids);
        }
#endif

#if 1   /* power off using power button */
        button = button_read_device();
        if (button & BUTTON_POWER) {
            power_off();
        }
#endif

#if 1   /* button info */
        snprintf(mystring, 64, "BUTTONS %08X, %s", button_read_device(),
                 headphones_inserted() ? "HP" : "hp");
        lcd_puts(0, line++, mystring);
#endif

#if 1   /* NAND debug */
        snprintf(mystring, 64, "NAND ID: %08X %08X", nand_ids[0], nand_ids[1]);
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "NAND ID: %08X %08X", nand_ids[2], nand_ids[3]);
        lcd_puts(0, line++, mystring);
#endif

#if 1
        snprintf(mystring, 64, "TIMER A:%08X B:%08X", (unsigned)TACNT, (unsigned)TBCNT);
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "TIMER C:%08X D:%08X", (unsigned)TCCNT, (unsigned)TDCNT);
        lcd_puts(0, line++, mystring);
#endif

        lcd_update();
    }
}


static int rds_decode(int line, struct si4700_dbg_info *nfo)
{
    unsigned short rdsdata[4];
    unsigned int pi, group, tp, pty, segment, abflag;
    static unsigned int af1 = 0, af2 = 0;
    static unsigned int day = 0, hour = 0, minute = 0;
    static unsigned int abflag_prev = -1;
    static char mystring[64];
    
    /* big RDS arrays */
    static char ps[9];
    static char rt[65];
    
    rdsdata[0] = nfo->regs[12];
    rdsdata[1] = nfo->regs[13];
    rdsdata[2] = nfo->regs[14];
    rdsdata[3] = nfo->regs[15];

    pi = rdsdata[0];
    group = (rdsdata[1] >> 11) & 0x1F;
    tp = (rdsdata[1] >> 10) & 1;
    pty = (rdsdata[1] >> 5) & 0x1F;
    
    switch (group) {
    
    case 0: /* group 0A: basic info */
        af1 = (rdsdata[2] >> 8) & 0xFF;
        af2 = (rdsdata[2] >> 0) & 0xFF;
        /* fall through */
    case 1: /* group 0B: basic info */
        segment = rdsdata[1] & 3;
        ps[segment * 2 + 0]  = (rdsdata[3] >> 8) & 0xFF;
        ps[segment * 2 + 1]  = (rdsdata[3] >> 0) & 0xFF;
        break;
    
    case 2: /* group 1A: programme item */
    case 3: /* group 1B: programme item */
        day = (rdsdata[3] >> 11) & 0x1F;
        hour = (rdsdata[3] >> 6) & 0x1F;
        minute = (rdsdata[3] >> 0) & 0x3F;
        break;
        
    case 4: /* group 2A: radio text */
        segment = rdsdata[1] & 0xF;
        abflag = (rdsdata[1] >> 4) & 1;
        if (abflag != abflag_prev) {
            memset(rt, '.', 64);
            abflag_prev = abflag;
        }
        rt[segment * 4 + 0] = (rdsdata[2] >> 8) & 0xFF;
        rt[segment * 4 + 1] = (rdsdata[2] >> 0) & 0xFF;
        rt[segment * 4 + 2] = (rdsdata[3] >> 8) & 0xFF;
        rt[segment * 4 + 3] = (rdsdata[3] >> 0) & 0xFF;
        break;
        
    case 5: /* group 2B: radio text */
        segment = rdsdata[1] & 0xF;
        abflag = (rdsdata[1] >> 4) & 1;
        if (abflag != abflag_prev) {
            memset(rt, '.', 64);
            abflag_prev = abflag;
        }
        rt[segment * 2 + 0] = (rdsdata[3] >> 8) & 0xFF;
        rt[segment * 2 + 1] = (rdsdata[3] >> 0) & 0xFF;
        break;
    
    default:
        break;
    }

    snprintf(mystring, 64, "PI:%04X,TP:%d,PTY:%2d,AF:%02X/%02X", pi, tp, pty, af1, af2);
    lcd_puts(0, line++, mystring);
    snprintf(mystring, 64, "PS:%s,ITEM:%02d-%02d:%02d", ps, day, hour, minute);
    lcd_puts(0, line++, mystring);
    snprintf(mystring, 64, "RT:%s", rt);
    lcd_puts(0, line++, mystring);
    
    return line;
}


