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
#include "samsung_yps3_usb.h"
#include "sdram-target.h"
#include "file_internal.h"
#include "storage.h"
#include "disk.h"
#include "file.h"
#include "dir.h"
#include "ftl-target.h"
#include "rb-loader.h"
#include "loader_strerror.h"

/* Rockbox is linked to the start of the SDRAM; the bootloader keeps its
   own buffers in the top MB */
#define LOAD_ADDR       ((unsigned char *)SDRAM_BASE)
#define LOAD_MAX_SIZE   (SDRAM_SIZE - 0x100000)

/* jump to an image in SDRAM with interrupts, caches and MPU off */
static void __attribute__((noreturn)) run_image(unsigned long addr)
{
    disable_irq();
    commit_discard_idcache();
    asm volatile(
        "mrc p15, 0, r0, c1, c0, 0  \n"
        "bic r0, r0, #0x1000        \n" /* I-cache */
        "bic r0, r0, #0x5           \n" /* D-cache, protection unit */
        "mcr p15, 0, r0, c1, c0, 0  \n"
        "mov r0, #0                 \n"
        "mcr p15, 0, r0, c7, c5, 0  \n" /* invalidate I-cache */
        "mcr p15, 0, r0, c7, c6, 0  \n" /* invalidate D-cache */
        "mov pc, %0                 \n"
        : : "r"(addr) : "r0", "memory");
    while (1);
}

#define LONG_DELAY  200000
#define SHORT_DELAY  50000
#define PAUSE_DELAY  50000

static inline void delay(unsigned int duration)
{
    volatile unsigned int i;
    for(i=0;i<duration;i++);
}

/* last start-up/loop step reached, shown by the crash dump */
volatile unsigned int dbg_stage;

/* Hang detector: a tick task that prints the current step on the bottom
   line of the screen if it has not changed for two seconds. */
static void dbg_tick(void)
{
    static unsigned int last_stage = 0xFFFFFFFF;
    static int count = 0;
    char str[32];

    if (dbg_stage != last_stage) {
        last_stage = dbg_stage;
        count = 0;
        return;
    }
    if (++count == 2 * HZ) {
        count = 0;
        snprintf(str, sizeof(str), "stuck at step %u", last_stage);
        lcd_puts(0, 25, str);
        lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    }
}

/* Show a progress number (0-7) on the three button LEDs, so that a hang or
   crash during start-up can be located without a working screen.
   bit 0 = menu/back LED, bit 1 = cursor LED, bit 2 = middle LED */
static void stage(unsigned int n)
{
    dbg_stage = n;
    PCON3 = (PCON3 & ~0x0000FF00) | 0x00001100;
    PCON4 = (PCON4 & ~0x00000F00) | 0x00000100;
    PDAT3 = (PDAT3 & ~0x0C) | ((n & 1) ? 0x04 : 0) | ((n & 2) ? 0x08 : 0);
    PDAT4 = (PDAT4 & ~0x04) | ((n & 4) ? 0x04 : 0);
}

/* Crash dump: the stock panic screen only shows the faulting pc, which is
   useless when the CPU ran off into the weeds. This replaces the (weak)
   SWI and reserved vector handlers and prints the registers instead. */
extern unsigned char _fiqstackend[];

void software_int_handler(void) __attribute__((naked));
void reserved_handler(void) __attribute__((naked, alias("software_int_handler")));

void software_int_handler(void)
{
    asm volatile(
        "stmdb  sp, {r0-r12}        \n"    /* registers go below the svc sp */
        "mov    r0, sp              \n"    /* r0 = svc sp */
        "mrs    r1, spsr            \n"    /* r1 = interrupted cpsr */
        "mov    r2, lr              \n"    /* r2 = lr */
        "msr    cpsr_c, #0xd2       \n"    /* IRQ mode */
        "mov    r3, sp              \n"    /* r3 = irq sp */
        "msr    cpsr_c, #0xd3       \n"    /* back to SVC mode */
        "ldr    sp, =_fiqstackend   \n"
        "b      crash_dump          \n"
    );
}

static void dump_words(int *line, const char *label, const unsigned int *p)
{
    char str[40];
    int i;

    snprintf(str, sizeof(str), "%s %08X", label, (unsigned)p);
    lcd_puts(0, (*line)++, str);
    for (i = 0; i < 12; i += 3) {
        snprintf(str, sizeof(str), "%08X %08X %08X", p[i], p[i + 1], p[i + 2]);
        lcd_puts(0, (*line)++, str);
    }
}

void crash_dump(unsigned int svc_sp, unsigned int spsr, unsigned int lr,
                unsigned int irq_sp) __attribute__((noreturn, used));
void crash_dump(unsigned int svc_sp, unsigned int spsr, unsigned int lr,
                unsigned int irq_sp)
{
    char str[40];
    const unsigned int *r = (const unsigned int *)(svc_sp - 13 * 4);
    int line = 0, i;

    lcd_clear_display();
    snprintf(str, sizeof(str), "SWI lr %08X spsr %08X", lr, spsr);
    lcd_puts(0, line++, str);
    snprintf(str, sizeof(str), "step %u", dbg_stage);
    lcd_puts(0, line++, str);
    for (i = 0; i < 12; i += 3) {
        snprintf(str, sizeof(str), "%08X %08X %08X", r[i], r[i + 1], r[i + 2]);
        lcd_puts(0, line++, str);
    }
    snprintf(str, sizeof(str), "r12 %08X", r[12]);
    lcd_puts(0, line++, str);
    if ((svc_sp >> 24) == 0x22) {
        dump_words(&line, "svc sp", (const unsigned int *)svc_sp);
    }
    if ((irq_sp >> 24) == 0x22) {
        dump_words(&line, "irq frame", (const unsigned int *)(irq_sp - 48));
    }
    lcd_update();
    while (1);
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

static unsigned char page_buf[NAND_MAX_PAGE + NAND_MAX_SPARE] SDRAM_BSS_ATTR;

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

/* Read a few locations whose content is known from the wiki, with each
   address mode, and show the first 8 bytes. Only the mode where page 127
   shows "220W" and page 130 shows "CUFD" at 0x20 is addressing correctly. */
static void nand_addr_test(void)
{
    static const struct {
        unsigned int page, col;
        const char *what;
    } tests[] = {
        {0,      0,      "p0"},
        {1,      0,      "p1"},
        {127,    0,      "p127 220W"},
        {130,    0x20,   "p130+20 CUFD"},
        {0,      4096,   "p0 spare"},
        {0x4F80, 0,      "p4F80 table"},
        {0x5880, 0,      "p5880 mp3"},
    };
    unsigned char buf[8];
    char str[48];
    int line = 0, mode, saved = nand_ll_addr_swap;
    unsigned int t, j;
    int col;

    lcd_clear_display();
    lcd_puts(0, line++, "NAND address test");
    for (mode = 1; mode >= 0; mode--) {
        nand_ll_addr_swap = mode;
        lcd_puts(0, line++, mode ? "-- BYTE mode --" : "-- PACK mode --");
        for (t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
            int rc = nand_ll_read_raw(0, tests[t].page, tests[t].col, buf, 8);
            col = snprintf(str, sizeof(str), "%d ", rc);
            for (j = 0; j < 8; j++) {
                col += snprintf(str + col, sizeof(str) - col, "%02X", buf[j]);
            }
            snprintf(str + col, sizeof(str) - col, " %s", tests[t].what);
            lcd_puts(0, line++, str);
        }
    }
    nand_ll_addr_swap = saved;
    lcd_puts(0, line + 1, "Please photograph, then any key");
    lcd_update();
    button_wait_press();
}

/* Serve NAND read requests from the PC until a key is pressed or the PC
   sends the exit request. Entered when the first request arrives. */
static void usbdump_mode(void)
{
    char str[40];
    long next_update = 0;

    lcd_clear_display();
    lcd_puts(0, 0, "USB NAND dump mode");
    lcd_puts(0, 1, "(read only)");
    lcd_puts(0, 3, "press any key to leave");
    lcd_update();

    while (true) {
        unsigned long exec;

        usbdump_poll();
        if (usbdump_exit_requested()) {
            break;
        }
        exec = usbdump_exec_requested();
        if (exec) {
            /* let the status stage of the request complete */
            long t = current_tick + HZ / 10;
            while (TIME_BEFORE(current_tick, t)) {
                usbdump_poll();
            }
            /* An uploaded bootloader copies itself to IRAM with data
               accesses and then executes it: run_image() turns off the
               caches so that it doesn't run stale instructions. */
            run_image(exec);
        }
        if (TIME_AFTER(current_tick, next_update)) {
            next_update = current_tick + HZ / 2;
            snprintf(str, sizeof(str), "requests: %u", usbdump_requests);
            lcd_puts(0, 5, str);
            lcd_update();
            if (button_read_device() & BUTTON_MAIN) {
                break;
            }
        }
    }
    while (button_read_device() & BUTTON_MAIN);
    lcd_clear_display();
}

/* Mount the FAT partitions through the FTL and list the root directory.
   Returns true if a filesystem was mounted. */
static bool fs_test(void)
{
    char str[48];
    int line = 0, rc;
    long t0 = current_tick;

    lcd_clear_display();
    lcd_puts(0, line++, "Filesystem test");
    lcd_update();

    rc = storage_init();
    snprintf(str, sizeof(str), "storage_init %d, %lu sectors", rc,
             (unsigned long)ftl_sector_count());
    lcd_puts(0, line++, str);
    bootlog("%s", str);
    ftl_debug_log(bootlog);

    lcd_update();
    if (rc == 0) {
        filesystem_init();
        rc = disk_mount_all();
        snprintf(str, sizeof(str), "mounted %d, %ld ms", rc,
                 (current_tick - t0) * 1000 / HZ);
        lcd_puts(0, line++, str);
        bootlog("%s", str);
        if (rc > 0) {
            DIR *dir = opendir("/");
            struct dirent *entry;
            while (dir && (entry = readdir(dir)) != NULL && line < 25) {
                struct dirinfo info = dir_get_info(dir, entry);
                snprintf(str, sizeof(str), "%s%s", entry->d_name,
                         (info.attribute & ATTR_DIRECTORY) ? "/" : "");
                lcd_puts(1, line++, str);
                bootlog("  %s", str);
            }
            if (dir) {
                closedir(dir);
            }
        }
    }
    lcd_update();
    sleep(3 * HZ);
    lcd_clear_display();
    return rc > 0;
}

/* Load and start Rockbox, unless a key is held */
/* busy wait: USEC_TIMER isn't running in this bootloader */
static void tick_wait(long ticks)
{
    long end = current_tick + ticks + (ticks ? 1 : 0);
    while (TIME_BEFORE(current_tick, end));
}

/* Reads the tuner's registers with different power-up delays and both
   polarities of P4.7, and logs DEVICEID (0x1242 for a Si4702/3). */
static void tuner_probe(void)
{
    unsigned char r[32];
    char hex[3 * 32 + 1];
    unsigned i, a;
    static const unsigned char addrs[] = {0x20, 0x22, 0xC0, 0xC6};

    /* P4.7 high powers the tuner (low reads all 0xFF) */
    PDAT4 &= ~(1 << 7);
    tick_wait(2);
    PDAT4 |= (1 << 7);
    tick_wait(10);
    for (a = 0; a < sizeof(addrs); a++) {
        int rc;
        memset(r, 0xEE, sizeof(r));
        rc = fmradio_i2c_read(addrs[a], r, sizeof(r));
        for (i = 0; i < sizeof(r); i++) {
            snprintf(hex + 3 * i, 4, "%02x ", r[i]);
        }
        bootlog("tuner i2c %02x rc %d: %s", addrs[a], rc, hex);
    }
    PDAT4 &= ~(1 << 7);
}

/* Measures the CPU and timer B (PCLK / 4800) speed against the USB frame
   counter (the host sends a frame every millisecond) */
typedef void (*loopfn_t)(void);
static void loop_iram(void)
{
    asm volatile("mov r0, #1000\n1: subs r0, r0, #1\n bne 1b" ::: "r0", "cc");
}
static loopfn_t cur_loop = loop_iram;

static void clock_measure(const char *label)
{
    unsigned long loops = 0, periods = 0, cp15;
    unsigned f, last, ms = 0;
    int oldlevel = disable_irq_save();

    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(cp15));
    TBCON = TBCON;
    SRCPND = INTMSK_TIMERB;
    last = USB_FNR & 0x7FF;
    while (ms < 200 && loops < 2000000) {
        cur_loop();
        loops++;
        if (SRCPND & INTMSK_TIMERB) {
            TBCON = TBCON;
            SRCPND = INTMSK_TIMERB;
            periods++;
        }
        f = USB_FNR & 0x7FF;
        ms += (f - last) & 0x7FF;
        last = f;
    }
    restore_irq(oldlevel);
    /* ~4 cycles per iteration: MHz = loops * 1000 * 4 / (ms * 1000) */
    /* timer B: PCLK / 16 / 300 / 100 per period (one tick at PCLK 48 MHz) */
    bootlog("%s: CLKCON %08lx CLKCON2 %08lx PLL0PMS %08lx cp15 %08lx: "
            "%u ms: CPU ~%lu MHz, %lu ticks/s (PCLK ~%lu MHz)", label,
            (unsigned long)CLKCON, (unsigned long)CLKCON2,
            (unsigned long)PLL0PMS, cp15, ms,
            ms ? loops * 4 / ms : 0, ms ? periods * 1000 / ms : 0,
            ms ? periods * 480 / ms : 0);
}

static void cp15_clocking(unsigned long mode)  /* 0 fastbus, 1 sync, 3 async */
{
    unsigned long r;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(r));
    r = (r & ~0xC0000000) | (mode << 30);
    asm volatile("mcr p15, 0, %0, c1, c0, 0\n nop\n nop\n nop\n nop" :: "r"(r));
}

static uint32_t sdram_loop[16] SDRAM_BSS_ATTR __attribute__((aligned(32)));

static void clock_probe(void)
{
    /* mov r0, #1000 (0x3e8 = 0xfa ror 30); subs r0, r0, #1; bne -2; bx lr */
    sdram_loop[0] = 0xe3a00ffa;
    sdram_loop[1] = 0xe2500001;
    sdram_loop[2] = 0x1afffffd;
    sdram_loop[3] = 0xe12fff1e;
    commit_discard_idcache();

    {
        unsigned long li, ld;
        asm volatile("mrc p15, 0, %0, c9, c0, 1" : "=r"(li));
        asm volatile("mrc p15, 0, %0, c9, c0, 0" : "=r"(ld));
        bootlog("lockdown: I %08lx D %08lx", li, ld);
    }
    cur_loop = loop_iram;
    clock_measure("loop in IRAM");
    cp15_clocking(0);
    clock_measure("fastbus (CPU = HCLK)");
    cp15_clocking(3);
    {
        unsigned long c2 = CLKCON2;
        int oldlevel = disable_irq_save();
        CLKCON2 = c2 | 0x100;
        restore_irq(oldlevel);
        clock_measure("CLKCON2 |= 0x100");
        oldlevel = disable_irq_save();
        CLKCON2 = c2;
        restore_irq(oldlevel);
    }
    cur_loop = (loopfn_t)sdram_loop;
    clock_measure("loop in SDRAM");

    {
        unsigned long i4, i5, i6, i7, d5, d6, d7;
        asm volatile("mrc p15, 0, %0, c6, c4, 1" : "=r"(i4));
        asm volatile("mrc p15, 0, %0, c6, c5, 1" : "=r"(i5));
        asm volatile("mrc p15, 0, %0, c6, c6, 1" : "=r"(i6));
        asm volatile("mrc p15, 0, %0, c6, c7, 1" : "=r"(i7));
        asm volatile("mrc p15, 0, %0, c6, c5, 0" : "=r"(d5));
        asm volatile("mrc p15, 0, %0, c6, c6, 0" : "=r"(d6));
        asm volatile("mrc p15, 0, %0, c6, c7, 0" : "=r"(d7));
        bootlog("regions: I4 %08lx I5 %08lx I6 %08lx I7 %08lx D5 %08lx "
                "D6 %08lx D7 %08lx", i4, i5, i6, i7, d5, d6, d7);
    }
    /* disable regions 5-7 */
    asm volatile("mov r0, #0\n"
                 " mcr p15, 0, r0, c6, c5, 1\n mcr p15, 0, r0, c6, c5, 0\n"
                 " mcr p15, 0, r0, c6, c6, 1\n mcr p15, 0, r0, c6, c6, 0\n"
                 " mcr p15, 0, r0, c6, c7, 1\n mcr p15, 0, r0, c6, c7, 0"
                 ::: "r0");
    commit_discard_idcache();
    cur_loop = loop_iram;
    clock_measure("regions 5-7 off, loop in IRAM");
    cur_loop = (loopfn_t)sdram_loop;
    clock_measure("regions 5-7 off, loop in SDRAM");
    cur_loop = loop_iram;
}

/* Plays silence through I2S with DMA like pcm-s5l8700.c and times it
   against the USB frame counter to find the real sample rate. */
static uint16_t audio_buf[16384] SDRAM_BSS_ATTR __attribute__((aligned(16)));

static void audio_measure(const char *label, int p, int m, int sd, int cdiv,
                          unsigned long txcon)
{
    unsigned last, f, ms = 0;
    unsigned long st = 0;

    I2STXCOM = 0x8;     /* tx mode, interface off */
    DMACOM0 = 5;
    PLL1PMS = (p << 16) | (m << 8) | sd;
    PLL1LCNT = 280;
    PLLCON |= 1 << 1;
    while ((PLLLOCK & (1 << 1)) == 0);
    CLKCON = (CLKCON & ~0xFF) | (2 << 5) | (1 << 4) | (cdiv - 1);
    PWRCON &= ~(1 << 6);
    I2STXCON = txcon;
    I2SCLKCON = 1;

    DMACON0 = (0 << 30) | (1 << 29) | (1 << 22) | (1 << 16);
    DMACOM0 = 7;
    DMABASE0 = (unsigned int)audio_buf;
    DMATCNT0 = sizeof(audio_buf) / 2 - 1;
    commit_dcache();
    last = USB_FNR & 0x7FF;
    DMACOM0 = 4;
    I2STXCOM = (1 << 3) | (1 << 2) | (1 << 1);
    while (ms < 3000) {
        st = DMAALLST;
        if (st & 1) break;
        f = USB_FNR & 0x7FF;
        ms += (f - last) & 0x7FF;
        last = f;
    }
    I2STXCOM = 0x8;
    DMACOM0 = 5;
    /* 16-bit stereo: 4 bytes per frame */
    bootlog("audio %s: PLL1 %d/%d/%d cdiv %d txcon %08lx: %u ms for %u frames "
            "-> %lu Hz (DMAALLST %08lx)", label, p, m, sd, cdiv, txcon, ms,
            (unsigned)(sizeof(audio_buf) / 4),
            ms ? (unsigned long)(sizeof(audio_buf) / 4) * 1000 / ms : 0, st);
}

/* Does writing DMABASE0/DMATCNT0 + DMACOM0 = 4 while a transfer runs
   queue a second transfer (like pcm-s5l8700.c assumes), or restart? */
static void audio_queue_test(void)
{
    unsigned last, f, ms = 0;
    unsigned long con_a, con_b, con_c, cnt_b, st = 0;
    long t;
    const unsigned half = sizeof(audio_buf) / 2;     /* bytes per part */

    I2STXCOM = 0x8;
    DMACOM0 = 5;
    DMACON0 = (0 << 30) | (1 << 29) | (1 << 22) | (1 << 16);
    DMACOM0 = 7;
    DMABASE0 = (unsigned int)audio_buf;
    DMATCNT0 = half / 2 - 1;
    commit_dcache();
    last = USB_FNR & 0x7FF;
    DMACOM0 = 4;
    I2STXCOM = (1 << 3) | (1 << 2) | (1 << 1);
    con_a = DMACON0;
    /* ~10 ms later, queue the second half */
    t = current_tick + 2;
    while (TIME_BEFORE(current_tick, t));
    cnt_b = DMACTCNT0;
    DMABASE0 = (unsigned int)audio_buf + half;
    DMATCNT0 = half / 2 - 1;
    DMACOM0 = 4;
    con_b = DMACON0;
    while (ms < 3000) {
        st = DMAALLST;
        if (st & 1) break;
        f = USB_FNR & 0x7FF;
        ms += (f - last) & 0x7FF;
        last = f;
    }
    con_c = DMACON0;
    I2STXCOM = 0x8;
    DMACOM0 = 5;
    bootlog("audio queue: con %08lx -> %08lx -> %08lx, ctcnt %lu at 20 ms, "
            "first WCOM after %u ms (one part = %u ms), DMACTCNT0 %lu",
            con_a, con_b, con_c, cnt_b, ms,
            (unsigned)(half / 4 * 1000 / 44100), (unsigned long)DMACTCNT0);
}

static void audio_probe(void)
{
    PCON7 = (PCON7 & ~(0x0FFFFF00)) | 0x02222200;
    memset(audio_buf, 0, sizeof(audio_buf));
    audio_measure("rockbox 44.1", 37, 151, 1, 9, 0x00000301);
    audio_queue_test();
}

/* CRC-32 (zlib) of files read through the FTL, to compare with the PC */
extern bool ftl_use_ecc;
extern bool nand_ll_use_dma;

static uint32_t crc32_buf(uint32_t crc, const unsigned char *p, int n)
{
    int k, b;
    for (k = 0; k < n; k++) {
        crc ^= p[k];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc;
}

/* per-4 KiB CRCs of one file, with the current DMA/ECC settings */
static void file_chunk_crcs(const char *name, const char *label)
{
    static unsigned char cbuf[4096] SDRAM_BSS_ATTR;
    char line[200];
    int fd = open(name, O_RDONLY), n, len = 0;
    if (fd < 0) return;
    len = snprintf(line, sizeof(line), "%s:", label);
    while ((n = read(fd, cbuf, sizeof(cbuf))) > 0 && len < 180) {
        len += snprintf(line + len, sizeof(line) - len, " %08lx",
                        (unsigned long)~crc32_buf(0xFFFFFFFF, cbuf, n));
    }
    close(fd);
    bootlog("%s", line);
}

static void file_crc_test(void)
{
    static const char * const files[] = {
        "/.rockbox/icons/tango_icons.12x12.bmp",
        "/.rockbox/icons/tango_icons_viewers.12x12.bmp",
        "/.rockbox/icons/viewers.6x8x16.bmp",
        "/.rockbox/codecs/mpa.codec",
        "/.rockbox/rockbox.yps3",
    };
    static unsigned char fbuf[4096] SDRAM_BSS_ATTR;
    unsigned i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        int fd = open(files[i], O_RDONLY);
        uint32_t crc = 0xFFFFFFFF;
        long total = 0;
        int n, k, b;
        if (fd < 0) {
            bootlog("crc %s: open %d", files[i], fd);
            continue;
        }
        while ((n = read(fd, fbuf, sizeof(fbuf))) > 0) {
            for (k = 0; k < n; k++) {
                crc ^= fbuf[k];
                for (b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
            total += n;
        }
        close(fd);
        bootlog("crc %08lx %ld %s", (unsigned long)~crc, total, files[i]);
    }
    {
        bool ecc = ftl_use_ecc, dma = nand_ll_use_dma;
        const char *f = "/.rockbox/icons/tango_icons.12x12.bmp";
        file_chunk_crcs(f, "dma+ecc");
        ftl_use_ecc = false;
        nand_ll_use_dma = false;
        /* drop cached sectors so the file is read again */
        file_chunk_crcs(f, "pio");
        ftl_use_ecc = ecc;
        nand_ll_use_dma = dma;
    }
}

static void try_boot(void)
{
    char str[48];
    int rc;

#ifdef YPS3_NO_AUTOBOOT
    /* development build: wait for an image uploaded over USB instead */
    bootlog("load: skipped (development bootloader)");
    return;
#endif
    if (button_read_device() & BUTTON_MAIN) {
        bootlog("key held, not booting");
        return;
    }
    lcd_clear_display();
    lcd_puts(0, 0, "Loading " BOOTFILE "...");
    lcd_update();
    rc = load_firmware(LOAD_ADDR, BOOTFILE, LOAD_MAX_SIZE);
    if (rc <= 0) {
        snprintf(str, sizeof(str), "load: %s", loader_strerror(rc));
        bootlog("%s", str);
        lcd_puts(0, 1, str);
        lcd_update();
        sleep(2 * HZ);
        lcd_clear_display();
        return;
    }
    bootlog("loaded %d bytes, starting", rc);
    lcd_puts(0, 1, "starting");
    lcd_update();
    run_image((unsigned long)LOAD_ADDR);
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

    nand_addr_test();

    while (true) {
        nand_decode_id(ids[bank], &geo);
        if (geo.blocks && page >= geo.blocks * geo.pagesperblock) {
            page = 0;
        }

        rc = nand_ll_read_raw(bank, page, 0, page_buf,
                              geo.pagesize + geo.sparesize);

        lcd_clear_display();
        line = 0;
        snprintf(str, sizeof(str), "bank %d ID %08X addr %s", bank, ids[bank],
                 nand_ll_addr_swap ? "BYTE" : "PACK");
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
        lcd_puts(0, line++, "U/D pg L/R blk MENU addr");
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
            /* toggle how the address is written (per byte / packed) */
            nand_ll_addr_swap = !nand_ll_addr_swap;
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
    int sdram_mb;
    
    // enable all peripherals
    PWRCON = 0;
    
    // disable all interrupts
    INTMSK = 0;
    
    // start with all GPIOs as input
    /* but keep P0.0 (backlight) and P0.1 (stay powered) as outputs, high */
    PCON0 = 0x5;
    PDAT0 |= 0x3;
    PCON1 = 0;
    PCON2 = 0;
    PCON3 = 0;
    PCON4 = 0;
    PCON5 = 0;
    PCON6 = 0;
    PCON7 = 0;
    stage(1);

    system_init();
    stage(2);
    kernel_init();
    
    /* crt0 starts main in SYS mode on its own stack; switching to SVC mode
       here (as this code did in 2009) moves main onto the IRQ stack, where
       the first timer interrupt overwrites its return addresses */
    enable_irq();
    tick_add_task(dbg_tick);
    stage(3);

    lcd_init();
    stage(4);
    
    lcd_clear_display();
    lcd_bitmap(rockboxlogo, 0, 160, BMPWIDTH_rockboxlogo, BMPHEIGHT_rockboxlogo);
    lcd_update();
    stage(5);

    power_init();
    stage(6);

    dbg_stage = 20;
    i2c_init();
    dbg_stage = 21;
    fmradio_i2c_init();
    dbg_stage = 22;
    adc_init();
    dbg_stage = 23;
    backlight_hw_init();
    dbg_stage = 24;
    button_init_device();

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
        
    // FM power
    dbg_stage = 25;
    dbg_stage = 26;
    si4700_init();
    dbg_stage = 27;
    tuner_power(true);
    si4700_set(RADIO_SLEEP, 0);
    si4700_set(RADIO_MUTE, 0);
    si4700_set(RADIO_REGION, 0);
    si4700_set(RADIO_FREQUENCY, fm_frequency);
    
    lcd_puts(0, 0, "Rockbox YP-S3 hardware test");

    // WM1800 codec configuration
    dbg_stage = 28;
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
    dbg_stage = 29;
    PDAT5 |= 1;

    dbg_stage = 30;
    nand_ll_init();
    for (i = 0; i < 4; i++) {
        nand_ll_reset(i);
        nand_ids[i] = nand_ll_read_id(i);
    }

    usbdump_init(nand_ids);

    dbg_stage = 31;
    lcd_puts(0, 1, "SDRAM init + test...");
    lcd_update();
    dbg_stage = 31;
    sdram_init();
    clock_measure("clocks");
    sdram_mb = sdram_test();
    usbdump_sdram_mb = sdram_mb;
    bootlog("sdram %d MB", sdram_mb);

    dbg_stage = 32;
    if (fs_test()) {
        file_crc_test();
        dbg_stage = 33;
        try_boot();
    }

    /* get rid of the logo and of start-up messages */
    lcd_clear_display();

    while (true)
    {
        dbg_stage = 40;
        if (usbdump_poll()) {
            usbdump_mode();
        }
        line = 1;

        dbg_stage = 41;
#if 1   /* enable to see GPIOs */
        snprintf(mystring, 64, "%02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned)PDAT0, (unsigned)PDAT1, (unsigned)PDAT2, (unsigned)PDAT3,
            (unsigned)PDAT4, (unsigned)PDAT5, (unsigned)PDAT6, (unsigned)PDAT7);
        lcd_puts(0, line++, mystring);
#endif

        dbg_stage = 42;
#if 1   /* enable this to see info about the RTC */
        rtc_read_datetime(&dt);
        snprintf(mystring, 64, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
            dt.tm_year + 1900, dt.tm_mon+1, dt.tm_mday,
            dt.tm_hour, dt.tm_min, dt.tm_sec);
        lcd_puts(0, line++, mystring);
#endif 

        dbg_stage = 43;
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

        dbg_stage = 44;
        si4700_dbg_info(&si4700_info);
        col = snprintf(mystring, 64, "FM: ");
        for (i = 0; i < 16; i++) {
            col += snprintf(mystring + col, 64, "%04X ", si4700_info.regs[i]);
            if (((i + 1) % 4) == 0) {
                lcd_puts(0, line++, mystring);
                col = 4;
            }
        }
        dbg_stage = 45;
        line = rds_decode(line, &si4700_info);
#endif

        dbg_stage = 46;
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

        dbg_stage = 47;
#if 1   /* enable this to see ADC info */
        snprintf(mystring, 64, "ADC: %04X %04X %04X %04X", 
            adc_read(0), adc_read(1), adc_read(2), adc_read(3));
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "ADC:USB %4d mV BAT %4d mV",
            (adc_read(0) * 6000) >> 10, (adc_read(2) * 4650) >> 10);
        lcd_puts(0, line++, mystring);
#endif

        dbg_stage = 48;
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

        dbg_stage = 49;
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

        dbg_stage = 50;
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

        dbg_stage = 51;
#if 1   /* SELECT opens the NAND probe */
        button = button_read_device();
        if (button & BUTTON_SELECT) {
            nand_probe(nand_ids);
        }
#endif

        dbg_stage = 52;
#if 1   /* power off using power button */
        button = button_read_device();
        if (button & BUTTON_POWER) {
            power_off();
        }
#endif

        dbg_stage = 53;
        snprintf(mystring, 64, "SDRAM: %d MB OK", sdram_mb);
        lcd_puts(0, line++, mystring);

#if 1   /* button info */
        snprintf(mystring, 64, "BUTTONS %08X, %s", button_read_device(),
                 headphones_inserted() ? "HP" : "hp");
        lcd_puts(0, line++, mystring);
#endif

        dbg_stage = 54;
#if 1   /* NAND debug */
        snprintf(mystring, 64, "NAND ID: %08X %08X", nand_ids[0], nand_ids[1]);
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "NAND ID: %08X %08X", nand_ids[2], nand_ids[3]);
        lcd_puts(0, line++, mystring);
#endif

        dbg_stage = 55;
#if 1
        snprintf(mystring, 64, "TIMER A:%08X B:%08X", (unsigned)TACNT, (unsigned)TBCNT);
        lcd_puts(0, line++, mystring);
        snprintf(mystring, 64, "TIMER C:%08X D:%08X", (unsigned)TCCNT, (unsigned)TDCNT);
        lcd_puts(0, line++, mystring);
#endif

        dbg_stage = 56;
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


