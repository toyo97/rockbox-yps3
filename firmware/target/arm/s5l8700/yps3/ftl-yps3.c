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

/*  Read-only implementation of the flash translation layer used by the
    original firmware of the Samsung YP-S3 (Samsung "Whimory" 2.2, the
    VFL/FTL part of "TFS4 Light").

    The layout was worked out from NAND dumps and from the behaviour of
    the original bootloader; see YPS3-PORT.md and the Python model in
    utils/s5l8700_dfu/yps3_vfl.py.

    Flash: one K9LBG08U0M, i.e. 2 dies ("banks") of 4096 blocks of 128
    pages of 4096 + 128 bytes. On-flash structures are big endian.

    VFL
    * virtual block v consists of the plane pair 2v / 2v+1 in each die.
      Bad blocks are replaced by reserved blocks 50 + i, where i is the
      index of the bad block in the list of the die's VFL context.
    * virtual page vpn = v * 256 + page * 2 + die, 8 KiB = plane 0 data,
      then plane 1 data, i.e. 16 sectors.
    * the VFL context of each die is page 0 of one of the physical blocks
      42/44/46/48, the valid one having the lowest non zero (inverted)
      sequence number in spare bytes 12-15.

    FTL
    * logical block = 256 virtual pages = 4096 sectors = 2 MiB.
    * the VFL context with the highest counter lists 10 FTL meta blocks.
      Their pages are FTL contexts (spare byte 16 == 0); the newest is the
      last written page with the lowest inverted sequence number.
    * the logical -> virtual block map is split in 8 chunks (chunk =
      lbn % 8, entry = lbn / 8), each stored at bytes 0x200-0x7ff of the
      meta page given by the context tables at 0xd0 (block) and 0x154
      (page).
    * up to 12 log blocks (context tables at 0x70: block, 0x88: lbn) hold
      recently written pages: spare bytes 12-15 = logical page number,
      spare bytes 16-31 = per sector status, 0xff = sector not in page.
      The newest copy of a page wins.

    TODO: use the ECC engine (data ECC: 9 bytes per sector at spare 0x28,
          meta ECC: 9 bytes at spare 2), DMA, and writing.
*/

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "config.h"
#include "system.h"
#include "panic.h"
#include "nand-target.h"
#include "ftl-target.h"
#include "sdram-target.h"

#ifndef BOOTLOADER
#include "kernel.h"
static struct mutex ftl_mtx;
#define FTL_LOCK()   mutex_lock(&ftl_mtx)
#define FTL_UNLOCK() mutex_unlock(&ftl_mtx)
#else
#define FTL_LOCK()
#define FTL_UNLOCK()
#endif

#define DIES                2
#define BLOCKS_PER_DIE      4096
#define PAGES_PER_BLOCK     128
#define PAGE_SIZE           4096
#define SECTOR_SIZE         512

#define VFL_CXT_FIRST       42
#define VFL_RESERVED_BASE   50
#define VFL_MAX_REMAP       ((PAGE_SIZE - 0x98) / 2)

#define VPAGES_PER_VBLOCK   256
#define SECTORS_PER_VPAGE   16
#define SECTORS_PER_LBLOCK  (SECTORS_PER_VPAGE * VPAGES_PER_VBLOCK)

#define FTL_META_BLOCKS     10
#define FTL_MAP_CHUNKS      8
#define FTL_MAP_ENTRIES     768     /* 0x600 bytes per chunk */
#define FTL_LOGS            12

static inline uint16_t be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static inline uint32_t be32(const uint8_t *p)
{
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint8_t buf[PAGE_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(4)));
static uint8_t spare[32] __attribute__((aligned(4)));

/* VFL */
static uint32_t vfl_counter[DIES];
static uint16_t vfl_meta[DIES][FTL_META_BLOCKS];
static uint16_t vfl_remap_count[DIES];
static uint16_t vfl_remap[DIES][256] SDRAM_BSS_ATTR; /* bad physical blocks */

/* FTL */
static uint16_t ftl_map[FTL_MAP_CHUNKS][FTL_MAP_ENTRIES] SDRAM_BSS_ATTR;
static uint32_t ftl_lblocks;

#define LOG_NONE 0xffff  /* page 255 is a valid log page */
#define LOG_UNUSED 0xffff

/* the 12 log slots, in the order of the context's tables */
struct ftl_log {
    uint16_t vblock;                        /* LOG_UNUSED: free slot */
    uint16_t lbn;
    uint32_t ec;                            /* erase count of vblock */
    uint32_t age;                           /* for the LRU merge victim */
    uint16_t nfree;                         /* next page offset to write */
    uint16_t nvalid;                        /* distinct logical pages */
    bool dirty;                             /* pages after the last commit */
    uint16_t page[VPAGES_PER_VBLOCK];       /* log page of each page,
                                               LOG_NONE if not in the log */
    uint16_t valid[VPAGES_PER_VBLOCK];      /* sectors held by the log */
};
static struct ftl_log ftl_logs[FTL_LOGS] SDRAM_BSS_ATTR;
static int ftl_nlogs;

/* newest context header and all 8 map chunks (bytes 0x200-0x7ff of their
   stream pages: map, reclaim bitmap, erase count table) */
static uint8_t ftl_hdr[0x200];
static uint8_t ftl_chunk[FTL_MAP_CHUNKS][0x600] SDRAM_BSS_ATTR;

static bool ftl_ok = false;

/* hardware ECC, off until validated on hardware (see ftl_ecc_selftest) */
bool ftl_use_ecc = false;
static uint8_t ecc_sector[SECTOR_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(4)));
static uint8_t ecc_spare[0x28 + 8 * 9] __attribute__((aligned(4)));
static unsigned long ecc_corrected, ecc_failed;

/* low level */

static int raw_read(int die, unsigned int block, unsigned int page,
                    unsigned int col, void *dst, unsigned int len)
{
    unsigned int row = (die * BLOCKS_PER_DIE + block) * PAGES_PER_BLOCK + page;
    return nand_ll_read_raw(0, row, col, dst, len);
}

static unsigned int vfl_phys_block(int die, unsigned int pblock)
{
    unsigned int i;
    for (i = 0; i < vfl_remap_count[die]; i++) {
        /* the list is padded with 0, block 0 is never remapped */
        if (vfl_remap[die][i] == pblock && pblock != 0) {
            return VFL_RESERVED_BASE + i;
        }
    }
    return pblock;
}

/* Reads part of a virtual page. 'offset' and 'len' are in bytes within the
   8 KiB virtual page and must not cross the plane boundary. */
static int vfl_read(uint32_t vpn, unsigned int offset, void *dst,
                    unsigned int len)
{
    unsigned int vblock = vpn / VPAGES_PER_VBLOCK;
    unsigned int rest = vpn % VPAGES_PER_VBLOCK;
    int die = rest & 1;
    unsigned int page = rest >> 1;
    unsigned int plane = offset / PAGE_SIZE;
    unsigned int pblock = vfl_phys_block(die, vblock * 2 + plane);
    unsigned int col = offset % PAGE_SIZE;

    if (!ftl_use_ecc || (col % SECTOR_SIZE) || (len % SECTOR_SIZE)) {
        return raw_read(die, pblock, page, col, dst, len);
    }

    /* whole sectors: read them and their ECC bytes, then correct */
    unsigned int first = col / SECTOR_SIZE, n = len / SECTOR_SIZE, i;
    if (raw_read(die, pblock, page, PAGE_SIZE, ecc_spare, sizeof(ecc_spare))) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint8_t *d = (uint8_t *)dst + i * SECTOR_SIZE;
        if (raw_read(die, pblock, page, col + i * SECTOR_SIZE, ecc_sector,
                     SECTOR_SIZE)) {
            return -1;
        }
        int rc = nand_ll_ecc_correct(ecc_sector,
                                     &ecc_spare[0x28 + 9 * (first + i)], true);
        if (rc < 0) {
            ecc_failed++;
        }
        else if (rc > 0) {
            ecc_corrected++;
        }
        memcpy(d, ecc_sector, SECTOR_SIZE);
    }
    return 0;
}

/* spare bytes 0-31 of plane 0 of a virtual page */
static int vfl_read_spare(uint32_t vpn, uint8_t *dst)
{
    unsigned int vblock = vpn / VPAGES_PER_VBLOCK;
    unsigned int rest = vpn % VPAGES_PER_VBLOCK;
    int die = rest & 1;

    return raw_read(die, vfl_phys_block(die, vblock * 2), rest >> 1,
                    PAGE_SIZE, dst, 32) == 0 ? 0 : -1;
}

static bool spare_empty(const uint8_t *sp)
{
    unsigned int i;
    for (i = 0; i < 32; i++) {
        if (sp[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static int vfl_open(void)
{
    int die;
    unsigned int i, blk;

    for (die = 0; die < DIES; die++) {
        uint32_t best = 0;
        int best_blk = -1;

        for (i = 0; i < 4; i++) {
            blk = VFL_CXT_FIRST + 2 * i;
            if (raw_read(die, blk, 0, PAGE_SIZE, spare, 32)) {
                continue;
            }
            uint32_t usn = be32(&spare[12]);
            if (usn == 0 || usn == 0xffffffff) {
                continue;
            }
            if (best_blk < 0 || usn < best) {
                best = usn;
                best_blk = blk;
            }
        }
        if (best_blk < 0) {
            return -1 - die;
        }
        if (raw_read(die, best_blk, 0, 0, buf, PAGE_SIZE)) {
            return -3;
        }
        vfl_counter[die] = be32(&buf[0]);
        for (i = 0; i < FTL_META_BLOCKS; i++) {
            vfl_meta[die][i] = be16(&buf[4 + 2 * i]);
        }
        vfl_remap_count[die] = be16(&buf[0x96]) * 2;
        if (vfl_remap_count[die] > 256) {
            return -4;
        }
        for (i = 0; i < vfl_remap_count[die]; i++) {
            vfl_remap[die][i] = be16(&buf[0x98 + 2 * i]);
        }
    }
    return 0;
}

/* last FTL context page of a meta block (binary search, like the OF) */
static int ftl_last_meta_page(unsigned int vblock, uint32_t *usn)
{
    int lo = 0, hi = VPAGES_PER_VBLOCK - 1, found = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (vfl_read_spare(vblock * VPAGES_PER_VBLOCK + mid, spare) == 0 &&
            !spare_empty(spare) && spare[16] == 0) {
            found = mid;
            *usn = be32(&spare[12]);
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    return found;
}

/*  Rebuilds a log's page table like the original firmware's _MakeLogMap:
    only pages up to the last one carrying a 0x00 status (the end of a
    write transaction) count; later pages are an interrupted write. */
static bool spare_committed(const uint8_t *sp)
{
    int s;
    if (be32(&sp[12]) >= VPAGES_PER_VBLOCK) {
        return false;
    }
    for (s = 0; s < SECTORS_PER_VPAGE; s++) {
        if (sp[16 + s] == 0x00) {
            return true;
        }
    }
    return false;
}

static int ftl_open_log(struct ftl_log *log)
{
    int p, last = -1;
    unsigned int s;

    memset(log->page, 0xff, sizeof(log->page));     /* LOG_NONE */
    memset(log->valid, 0, sizeof(log->valid));
    log->nvalid = 0;
    log->dirty = false;
    log->age = 0;

    for (p = VPAGES_PER_VBLOCK - 1; p >= 0; p--) {
        if (vfl_read_spare(log->vblock * VPAGES_PER_VBLOCK + p, spare)) {
            return -1;
        }
        if (spare_empty(spare)) {
            continue;
        }
        if (spare_committed(spare)) {
            last = p;
            break;
        }
        log->dirty = true;      /* written after the last commit */
    }
    log->nfree = last + 1;
    /* the original firmware also wants the 3 offsets after it erased */
    for (p = last + 1; p < last + 4 && p < VPAGES_PER_VBLOCK; p++) {
        if (vfl_read_spare(log->vblock * VPAGES_PER_VBLOCK + p, spare)) {
            return -1;
        }
        if (!spare_empty(spare)) {
            log->dirty = true;
        }
    }

    for (p = 0; p <= last; p++) {
        if (vfl_read_spare(log->vblock * VPAGES_PER_VBLOCK + p, spare)) {
            return -1;
        }
        if (spare_empty(spare)) {
            continue;
        }
        uint32_t lp = be32(&spare[12]);
        if (lp >= VPAGES_PER_VBLOCK) {
            continue;
        }
        if (log->page[lp] == LOG_NONE) {
            log->nvalid++;
        }
        log->page[lp] = p;
        log->valid[lp] = 0;
        for (s = 0; s < SECTORS_PER_VPAGE; s++) {
            if (spare[16 + s] != 0xff) {
                log->valid[lp] |= 1 << s;
            }
        }
    }
    return 0;
}

static int ftl_open(void)
{
    const uint16_t *meta;
    unsigned int i;
    int best_page = -1;
    unsigned int best_block = 0;
    uint32_t best_usn = 0;

    meta = vfl_counter[0] >= vfl_counter[1] ? vfl_meta[0] : vfl_meta[1];
    for (i = 0; i < FTL_META_BLOCKS; i++) {
        uint32_t usn;
        int page = ftl_last_meta_page(meta[i], &usn);
        if (page >= 0 && (best_page < 0 || usn < best_usn)) {
            best_page = page;
            best_block = meta[i];
            best_usn = usn;
        }
    }
    if (best_page < 0) {
        return -10;
    }

    /* the context header is the first 0x200 bytes */
    if (vfl_read(best_block * VPAGES_PER_VBLOCK + best_page, 0, buf, 0x200)) {
        return -11;
    }
    memcpy(ftl_hdr, buf, sizeof(ftl_hdr));

    uint16_t map_block[FTL_MAP_CHUNKS], map_page[FTL_MAP_CHUNKS];
    for (i = 0; i < FTL_MAP_CHUNKS; i++) {
        map_block[i] = be16(&buf[0xd0 + 2 * i]);
        map_page[i] = be16(&buf[0x154 + 2 * i]);
    }

    ftl_nlogs = 0;
    for (i = 0; i < FTL_LOGS; i++) {
        ftl_logs[i].vblock = be16(&buf[0x70 + 2 * i]);
        ftl_logs[i].lbn = be16(&buf[0x88 + 2 * i]);
        ftl_logs[i].ec = be32(&buf[0xa0 + 4 * i]);
        if (ftl_logs[i].vblock != LOG_UNUSED) {
            ftl_nlogs++;
        }
    }

    for (i = 0; i < FTL_MAP_CHUNKS; i++) {
        if (vfl_read(map_block[i] * VPAGES_PER_VBLOCK + map_page[i], 0x200,
                     buf, FTL_MAP_ENTRIES * 2)) {
            return -12;
        }
        memcpy(ftl_chunk[i], buf, 0x600);
        unsigned int j;
        for (j = 0; j < FTL_MAP_ENTRIES; j++) {
            ftl_map[i][j] = be16(&buf[2 * j]);
        }
    }

    for (i = 0; i < FTL_LOGS; i++) {
        if (ftl_logs[i].vblock != LOG_UNUSED && ftl_open_log(&ftl_logs[i])) {
            return -13;
        }
    }

    /* count mapped logical blocks */
    ftl_lblocks = 0;
    while (ftl_lblocks < FTL_MAP_CHUNKS * FTL_MAP_ENTRIES &&
           ftl_map[ftl_lblocks % FTL_MAP_CHUNKS][ftl_lblocks / FTL_MAP_CHUNKS]
           < BLOCKS_PER_DIE / 2) {
        ftl_lblocks++;
    }
    return 0;
}

void ftl_ecc_selftest(void (*logf_fn)(const char *fmt, ...), int pages);
void ftl_ecc_encode_selftest(void (*logf_fn)(const char *fmt, ...), int pages);
bool ftl_dma_selftest(void (*logf_fn)(const char *fmt, ...));

static int ftl_init_rc;
static bool wcache_ready;           /* the write cache, see ftl_write() */
static void ftl_write_setup(void);
static char selftest_log[4][80];
static int selftest_n;

static void selftest_logf(const char *fmt, ...)
{
    va_list ap;
    if (selftest_n >= 4) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(selftest_log[selftest_n++], sizeof(selftest_log[0]), fmt, ap);
    va_end(ap);
}

uint32_t ftl_init(void)
{
    int rc;

    nand_ll_init();
    nand_ll_reset(0);
    wcache_ready = false;

    rc = vfl_open();
    if (rc) {
        ftl_init_rc = rc;
        return 1;
    }
    rc = ftl_open();
    if (rc) {
        ftl_init_rc = rc;
        return 2;
    }
    ftl_ok = true;
#ifndef BOOTLOADER
    mutex_init(&ftl_mtx);
#endif
    ftl_write_setup();

    /* DMA and ECC are only used if they work as expected on this unit */
    selftest_n = 0;
#ifndef YPS3_NO_SELFTEST
    ftl_dma_selftest(selftest_logf);
    ftl_ecc_selftest(selftest_logf, 32);
#ifdef BOOTLOADER
    ftl_ecc_encode_selftest(selftest_logf, 8);
#endif
#endif
    return 0;
}

/* Decodes the sectors of 'pages' data pages with the ECC engine and counts
   what it reports, without using the result. Reading valid data, a correctly
   driven engine reports (almost) no errors. */
void ftl_ecc_selftest(void (*logf_fn)(const char *fmt, ...), int pages)
{
    static uint8_t raw[SECTOR_SIZE] __attribute__((aligned(4)));
    int clean = 0, corrected = 0, failed = 0, changed = 0, p, s;

    for (p = 0; p < pages; p++) {
        /* first pages of the first logical blocks, plane 0 of die 0 */
        unsigned int lbn = p / 4;
        uint16_t vb = ftl_map[lbn % FTL_MAP_CHUNKS][lbn / FTL_MAP_CHUNKS];
        unsigned int page = 8 + (p % 4) * 16;
        if (vb >= BLOCKS_PER_DIE / 2) {
            continue;
        }
        unsigned int pblock = vfl_phys_block(0, vb * 2);
        if (raw_read(0, pblock, page, PAGE_SIZE, ecc_spare, sizeof(ecc_spare))) {
            continue;
        }
        for (s = 0; s < 8; s++) {
            if (raw_read(0, pblock, page, s * SECTOR_SIZE, ecc_sector,
                         SECTOR_SIZE)) {
                continue;
            }
            memcpy(raw, ecc_sector, SECTOR_SIZE);
            int rc = nand_ll_ecc_correct(ecc_sector, &ecc_spare[0x28 + 9 * s],
                                         true);
            if (rc < 0) failed++;
            else if (rc > 0) corrected++;
            else clean++;
            if (memcmp(raw, ecc_sector, SECTOR_SIZE)) changed++;
        }
    }
    /* a correctly driven engine finds few errors in valid data */
    ftl_use_ecc = (clean > 0 && failed == 0 && corrected * 8 <= clean);
    logf_fn("ecc test: clean %d corrected %d failed %d changed %d -> ecc %s",
            clean, corrected, failed, changed, ftl_use_ecc ? "on" : "off");
}

/* Encodes sectors and metadata already on the flash and compares the ECC
   with what the original firmware stored, for both SIZE_SEL values.
   Read only: validates the encoder needed for writing. */
void ftl_ecc_encode_selftest(void (*logf_fn)(const char *fmt, ...), int pages)
{
    static uint8_t meta[SECTOR_SIZE] __attribute__((aligned(16)));
    unsigned char mfirst[9] = {0}, mstored[9] = {0};
    unsigned char e[9];
    int n = 0, s0 = 0, s1 = 0, nm = 0, m0 = 0, m1 = 0, p, s;
    unsigned char first[9] = {0}, firstgot[9] = {0};

    for (p = 0; p < pages; p++) {
        unsigned int lbn = p / 4;
        uint16_t vb = ftl_map[lbn % FTL_MAP_CHUNKS][lbn / FTL_MAP_CHUNKS];
        unsigned int page = 8 + (p % 4) * 16;
        if (vb >= BLOCKS_PER_DIE / 2)
            continue;
        unsigned int pblock = vfl_phys_block(0, vb * 2);
        if (raw_read(0, pblock, page, PAGE_SIZE, ecc_spare, sizeof(ecc_spare)))
            continue;
        /* metadata: spare 12..31, its ECC at spare 2..10; the engine may
           read a whole 512 byte block: try 0xFF and 0x00 padding */
        nm++;
        if (nm == 1)
            memcpy(mstored, &ecc_spare[2], 9);
        memset(meta, 0xff, sizeof(meta));
        memcpy(meta, &ecc_spare[12], 20);
        if (!nand_ll_ecc_encode(meta, e, 0) && !memcmp(e, &ecc_spare[2], 9))
            m0++;
        if (nm == 1)
            memcpy(mfirst, e, 9);
        memset(meta, 0x00, sizeof(meta));
        memcpy(meta, &ecc_spare[12], 20);
        if (!nand_ll_ecc_encode(meta, e, 0) && !memcmp(e, &ecc_spare[2], 9))
            m1++;
        for (s = 0; s < 8; s++) {
            if (raw_read(0, pblock, page, s * SECTOR_SIZE, ecc_sector,
                         SECTOR_SIZE))
                continue;
            n++;
            if (!nand_ll_ecc_encode(ecc_sector, e, 0) &&
                !memcmp(e, &ecc_spare[0x28 + 9 * s], 9))
                s0++;
            if (!nand_ll_ecc_encode(ecc_sector, e, 1)) {
                if (!memcmp(e, &ecc_spare[0x28 + 9 * s], 9))
                    s1++;
                if (n == 1) {
                    memcpy(first, &ecc_spare[0x28], 9);
                    memcpy(firstgot, e, 9);
                }
            }
        }
    }
    logf_fn("ecc enc: sectors %d sel0 %d sel1 %d; meta %d padFF %d pad00 %d",
            n, s0, s1, nm, m0, m1);
    logf_fn("ecc enc meta: stored %02x%02x%02x%02x%02x got %02x%02x%02x%02x%02x",
            mstored[0], mstored[1], mstored[2], mstored[3], mstored[4],
            mfirst[0], mfirst[1], mfirst[2], mfirst[3], mfirst[4]);
    (void)first; (void)firstgot;
}

/* Compares DMA and programmed I/O reads of some pages and enables DMA if
   they match. Returns true if DMA is in use. */
bool ftl_dma_selftest(void (*logf_fn)(const char *fmt, ...))
{
    static uint8_t pio[PAGE_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(16)));
    int p, same = 0, diff = 0, err = 0;

    for (p = 0; p < 16; p++) {
        unsigned int lbn = p;
        uint16_t vb = ftl_map[lbn % FTL_MAP_CHUNKS][lbn / FTL_MAP_CHUNKS];
        if (vb >= BLOCKS_PER_DIE / 2) {
            continue;
        }
        unsigned int pblock = vfl_phys_block(p & 1, vb * 2 + ((p >> 1) & 1));
        nand_ll_use_dma = false;
        if (raw_read(p & 1, pblock, p, 0, pio, PAGE_SIZE)) {
            err++;
            continue;
        }
        nand_ll_use_dma = true;
        if (raw_read(p & 1, pblock, p, 0, buf, PAGE_SIZE)) {
            err++;
            nand_ll_use_dma = false;
            continue;
        }
        nand_ll_use_dma = false;
        if (memcmp(pio, buf, PAGE_SIZE)) diff++;
        else same++;
    }
    nand_ll_use_dma = (same > 0 && diff == 0 && err == 0);
    logf_fn("dma test: same %d diff %d err %d -> dma %s", same, diff, err,
            nand_ll_use_dma ? "on" : "off");
    return nand_ll_use_dma;
}

void ftl_debug_log(void (*logf_fn)(const char *fmt, ...))
{
    int i;
    logf_fn("ftl rc %d ok %d lblocks %lu", ftl_init_rc, ftl_ok,
            (unsigned long)ftl_lblocks);
    for (i = 0; i < DIES; i++) {
        logf_fn("vfl die %d counter %lx meta0 %x remap %d", i,
                (unsigned long)vfl_counter[i], vfl_meta[i][0],
                vfl_remap_count[i]);
    }
    for (i = 0; i < selftest_n; i++) {
        logf_fn("%s", selftest_log[i]);
    }
    logf_fn("ecc %d corrected %lu failed %lu", ftl_use_ecc, ecc_corrected,
            ecc_failed);
    logf_fn("map %x %x %x %x logs %d first %x/%x", ftl_map[0][0],
            ftl_map[1][0], ftl_map[2][0], ftl_map[3][0], ftl_nlogs,
            ftl_logs[0].vblock, ftl_logs[0].lbn);
}

uint32_t ftl_sector_count(void)
{
    return ftl_lblocks * SECTORS_PER_LBLOCK;
}

static struct ftl_log *ftl_find_log(unsigned int lbn)
{
    int i;
    for (i = 0; i < FTL_LOGS; i++) {
        if (ftl_logs[i].vblock != LOG_UNUSED && ftl_logs[i].lbn == lbn) {
            return &ftl_logs[i];
        }
    }
    return NULL;
}

static uint32_t ftl_read_nand(uint32_t sector, uint32_t count, void *buffer)
{
    uint8_t *dst = buffer;

    if (!ftl_ok) {
        return 1;
    }
    nand_set_active();

    while (count) {
        unsigned int lbn = sector / SECTORS_PER_LBLOCK;
        unsigned int vpage = (sector / SECTORS_PER_VPAGE) % VPAGES_PER_VBLOCK;
        unsigned int sec = sector % SECTORS_PER_VPAGE;
        const struct ftl_log *log = ftl_find_log(lbn);
        uint32_t vpn;
        unsigned int n;

        if (log && log->page[vpage] != LOG_NONE &&
            (log->valid[vpage] & (1 << sec))) {
            vpn = log->vblock * VPAGES_PER_VBLOCK + log->page[vpage];
            n = 1;
        }
        else {
            uint16_t vblock;
            if (lbn >= FTL_MAP_CHUNKS * FTL_MAP_ENTRIES) {
                return 2;
            }
            vblock = ftl_map[lbn % FTL_MAP_CHUNKS][lbn / FTL_MAP_CHUNKS];
            if (vblock >= BLOCKS_PER_DIE / 2) {
                /* unmapped: reads as erased */
                memset(dst, 0xff, SECTOR_SIZE);
                dst += SECTOR_SIZE;
                sector++;
                count--;
                continue;
            }
            vpn = vblock * VPAGES_PER_VBLOCK + vpage;
            /* read as many sectors as possible from this plane page, as
               long as none of them is held by the log */
            n = 1;
            while (n < count && ((sec + n) % 8) != 0 &&
                   !(log && log->page[vpage] != LOG_NONE &&
                     (log->valid[vpage] & (1 << (sec + n))))) {
                n++;
            }
        }

        if (vfl_read(vpn, sec * SECTOR_SIZE, dst, n * SECTOR_SIZE)) {
            return 3;
        }
        dst += n * SECTOR_SIZE;
        sector += n;
        count -= n;
    }
    return 0;
}

/*  Writing, compatible with the original firmware (see YPS3-PORT.md and
    the write specification derived from its Whimory 2.2 code):
    * every write is a full 16 sector virtual page appended to the log
      block of its logical block, spare: logical page at 12-15, sector
      statuses 0x0f with 0x00 on sector 15 (each page ends a transaction),
      log page offsets keep the parity (die) of the logical page;
    * a new log takes a block from the free ring (lowest erase count); with
      all 12 logs in use, or a full log, a "simple merge" copies the newest
      pages into a fresh block that becomes the data block, and the old
      data and log blocks go back to the free ring;
    * whenever the logs or the map change, a new context page (header +
      the affected map chunk) is written to that chunk's meta stream.
    No wear levelling (the original firmware does it when it runs). Only
    the FAT32 music partition can be written. Any NAND error stops
    writing until the next start. */

bool ftl_write_enabled = false;
static bool ftl_write_failed;
static uint32_t ftl_write_first, ftl_write_end;   /* writable sectors */
static uint32_t ftl_age_counter;
unsigned long ftl_stat_pages, ftl_stat_merges, ftl_stat_cxt, ftl_stat_erases;
unsigned long ftl_stat_copymerges;

static uint8_t wbuf[2 * PAGE_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(16)));
static uint8_t cbuf[2 * PAGE_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(16)));
static uint8_t pbuf[PAGE_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(16)));
static uint8_t eccblock[SECTOR_SIZE] SDRAM_BSS_ATTR __attribute__((aligned(16)));
static uint8_t wspare[128] __attribute__((aligned(16)));

/* the ECC the original firmware stores for a sector it doesn't write */
static const uint8_t ecc_blank[9] =
    { 0x1b, 0xb1, 0x4e, 0x81, 0xff, 0xd5, 0x38, 0x4e, 0x0a };

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8; p[1] = v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* header fields */
#define H_USN           0x000
#define H_SPARE_VB      0x004
#define H_SPARE_EC      0x008
#define H_CACHED        0x00c
#define H_NFREE         0x00e
#define H_HEAD          0x010
#define H_WL_STATIC     0x012
#define H_WL_META       0x014
#define H_RING          0x016
#define H_RING_EC       0x034
#define H_LOG_VB        0x070
#define H_LOG_LBN       0x088
#define H_LOG_EC        0x0a0
#define H_STREAM_VB     0x0d0
#define H_STREAM_PG     0x154
#define RING_SIZE       15
/* chunk fields (offsets in ftl_chunk, i.e. page offset - 0x200) */
#define C_MAP           0x000
#define C_STREAM_EC     0x204
#define C_ECTBL         0x300

int ftl_write_last_error;

static int wfail(int rc)
{
    ftl_write_failed = true;
    ftl_write_last_error = rc;
    return rc;
}

/* programs one plane page; 'data' 4096 bytes (or NULL), 'mask' sectors */
static int vfl_program_plane(int die, unsigned int pblock, unsigned int page,
                             const uint8_t *data, unsigned int mask,
                             const uint8_t *meta)
{
    unsigned int s;
    uint32_t row = (die * BLOCKS_PER_DIE + pblock) * PAGES_PER_BLOCK + page;

    memset(wspare, 0xff, sizeof(wspare));
    wspare[1] = 0x00;
    memcpy(&wspare[12], meta, 20);
    for (s = 0; s < 8; s++) {
        if (data && (mask & (1 << s))) {
            memcpy(&pbuf[s * SECTOR_SIZE], data + s * SECTOR_SIZE, SECTOR_SIZE);
            if (nand_ll_ecc_encode(&pbuf[s * SECTOR_SIZE],
                                   &wspare[40 + 9 * s], 0)) {
                return -10;
            }
        }
        else {
            memset(&pbuf[s * SECTOR_SIZE], 0xff, SECTOR_SIZE);
            memcpy(&wspare[40 + 9 * s], ecc_blank, 9);
        }
    }
    memset(eccblock, 0xff, sizeof(eccblock));
    memcpy(eccblock, meta, 20);
    if (nand_ll_ecc_encode(eccblock, &wspare[2], 0)) {
        return -10;
    }
    return nand_ll_program_page(0, row, pbuf, wspare, sizeof(wspare));
}

/* programs a virtual page: data is 8 KiB, mask has 16 sector bits; the
   same 20 metadata bytes go to both planes like the original firmware */
static int vfl_program(uint32_t vpn, const uint8_t *data, unsigned int mask,
                       const uint8_t *meta)
{
    unsigned int vblock = vpn / VPAGES_PER_VBLOCK;
    unsigned int rest = vpn % VPAGES_PER_VBLOCK;
    int die = rest & 1, plane, rc;

    for (plane = 0; plane < 2; plane++) {
        rc = vfl_program_plane(die, vfl_phys_block(die, vblock * 2 + plane),
                               rest >> 1, data ? data + plane * PAGE_SIZE : NULL,
                               (mask >> (8 * plane)) & 0xff, meta);
        if (rc) {
            return rc;
        }
    }
    return 0;
}

/* erases a virtual block: both planes in both dies */
static int vfl_erase(unsigned int vblock)
{
    int die, plane, rc;

    for (die = 0; die < DIES; die++) {
        for (plane = 0; plane < 2; plane++) {
            unsigned int pb = vfl_phys_block(die, vblock * 2 + plane);
            rc = nand_ll_erase_block(0, (die * BLOCKS_PER_DIE + pb) *
                                        PAGES_PER_BLOCK);
            if (rc) {
                return rc;
            }
        }
    }
    ftl_stat_erases++;
    return 0;
}

/* reads a whole virtual page (ECC corrected when enabled) and its plane 0
   metadata. Returns 1 if the page is blank. */
static int vfl_read_vpage(uint32_t vpn, uint8_t *data, uint8_t *meta)
{
    if (vfl_read_spare(vpn, spare)) {
        return -1;
    }
    if (spare_empty(spare)) {
        return 1;
    }
    memcpy(meta, &spare[12], 20);
    if (vfl_read(vpn, 0, data, PAGE_SIZE) ||
        vfl_read(vpn, PAGE_SIZE, data + PAGE_SIZE, PAGE_SIZE)) {
        return -1;
    }
    return 0;
}

static uint16_t map_get(unsigned int lbn)
{
    return ftl_map[lbn % FTL_MAP_CHUNKS][lbn / FTL_MAP_CHUNKS];
}

static void map_set(unsigned int lbn, uint16_t vb, uint32_t ec)
{
    uint8_t *c = ftl_chunk[lbn % FTL_MAP_CHUNKS];
    unsigned int i = lbn / FTL_MAP_CHUNKS;
    ftl_map[lbn % FTL_MAP_CHUNKS][i] = vb;
    put16(&c[C_MAP + 2 * i], vb);
    c[C_ECTBL + 3 * i] = ec >> 16;
    c[C_ECTBL + 3 * i + 1] = ec >> 8;
    c[C_ECTBL + 3 * i + 2] = ec;
}

static uint32_t ec_get(unsigned int lbn)
{
    const uint8_t *c = ftl_chunk[lbn % FTL_MAP_CHUNKS];
    unsigned int i = lbn / FTL_MAP_CHUNKS;
    return (c[C_ECTBL + 3 * i] << 16) | (c[C_ECTBL + 3 * i + 1] << 8) |
           c[C_ECTBL + 3 * i + 2];
}

/* takes the free block with the lowest erase count and erases it */
static int get_free_vb(uint16_t *vb, uint32_t *ec)
{
    unsigned int n = be16(&ftl_hdr[H_NFREE]);
    unsigned int head = be16(&ftl_hdr[H_HEAD]);
    unsigned int i, best = head, idx;
    uint32_t best_ec = 0xffffffff;

    if (n == 0 || head >= RING_SIZE) {
        return -20;
    }
    for (i = 0; i < n; i++) {
        idx = (head + i) % RING_SIZE;
        uint32_t e = be32(&ftl_hdr[H_RING_EC + 4 * idx]);
        if (e < best_ec) {
            best_ec = e;
            best = idx;
        }
    }
    /* swap it to the head of the ring */
    uint16_t bvb = be16(&ftl_hdr[H_RING + 2 * best]);
    put16(&ftl_hdr[H_RING + 2 * best], be16(&ftl_hdr[H_RING + 2 * head]));
    put32(&ftl_hdr[H_RING_EC + 4 * best], be32(&ftl_hdr[H_RING_EC + 4 * head]));
    put16(&ftl_hdr[H_RING + 2 * head], bvb);
    put32(&ftl_hdr[H_RING_EC + 4 * head], best_ec + 1);
    if (vfl_erase(bvb)) {
        return -21;
    }
    *vb = bvb;
    *ec = best_ec + 1;
    put16(&ftl_hdr[H_HEAD], (head + 1) % RING_SIZE);
    put16(&ftl_hdr[H_NFREE], n - 1);
    return 0;
}

static void add_free_vb(uint16_t vb, uint32_t ec)
{
    unsigned int n = be16(&ftl_hdr[H_NFREE]);
    unsigned int tail = (be16(&ftl_hdr[H_HEAD]) + n) % RING_SIZE;
    put16(&ftl_hdr[H_RING + 2 * tail], vb);
    put32(&ftl_hdr[H_RING_EC + 4 * tail], ec);
    put16(&ftl_hdr[H_NFREE], n + 1);
}

/* writes the context header with map chunk 'c' to the next page of the
   chunk's meta stream, like _StoreFTLCxt(1) */
static int store_cxt(unsigned int c)
{
    unsigned int svb = be16(&ftl_hdr[H_STREAM_VB + 2 * c]);
    unsigned int spg = be16(&ftl_hdr[H_STREAM_PG + 2 * c]);
    uint32_t vpn = svb * VPAGES_PER_VBLOCK + spg + 1;
    uint8_t meta[20];
    int i;

    if (vpn % VPAGES_PER_VBLOCK == 0) {
        /* the stream's block is full: continue in the spare meta block,
           the old one becomes the spare */
        uint16_t nvb = be32(&ftl_hdr[H_SPARE_VB]);
        uint32_t tmp = be32(&ftl_hdr[H_SPARE_EC]);
        put32(&ftl_hdr[H_SPARE_VB], svb);
        if (vfl_erase(nvb)) {
            return -30;
        }
        put32(&ftl_hdr[H_SPARE_EC], be32(&ftl_chunk[c][C_STREAM_EC]));
        put32(&ftl_chunk[c][C_STREAM_EC], tmp + 1);
        vpn = nvb * VPAGES_PER_VBLOCK;
    }
    put16(&ftl_hdr[H_STREAM_VB + 2 * c], vpn / VPAGES_PER_VBLOCK);
    put16(&ftl_hdr[H_STREAM_PG + 2 * c], vpn % VPAGES_PER_VBLOCK);
    put32(&ftl_hdr[H_USN], be32(&ftl_hdr[H_USN]) - 1);
    put16(&ftl_hdr[H_CACHED], c);
    for (i = 0; i < FTL_LOGS; i++) {
        put16(&ftl_hdr[H_LOG_VB + 2 * i], ftl_logs[i].vblock);
        put16(&ftl_hdr[H_LOG_LBN + 2 * i], ftl_logs[i].lbn);
        put32(&ftl_hdr[H_LOG_EC + 4 * i], ftl_logs[i].ec);
    }

    memset(cbuf, 0xff, sizeof(cbuf));
    memcpy(cbuf, ftl_hdr, 0x200);
    memcpy(cbuf + 0x200, ftl_chunk[c], 0x600);
    memset(meta, 0xff, sizeof(meta));
    memcpy(meta, &ftl_hdr[H_USN], 4);
    meta[4] = 0x00;
    ftl_stat_cxt++;
    return vfl_program(vpn, cbuf, 0x000f, meta);
}

static void log_init(struct ftl_log *log)
{
    memset(log->page, 0xff, sizeof(log->page));
    memset(log->valid, 0, sizeof(log->valid));
    log->nfree = 0;
    log->nvalid = 0;
    log->dirty = false;
}

/* _DoSimpleMerge: the newest copy of every page into a fresh block, which
   becomes the data block; the old data and log blocks are freed */
static int simple_merge(struct ftl_log *log)
{
    unsigned int lbn = log->lbn, p;
    uint16_t dvb = map_get(lbn), nvb;
    uint32_t nec;
    uint8_t meta[20];
    int rc;

    rc = get_free_vb(&nvb, &nec);
    if (rc) {
        return rc;
    }
    for (p = 0; p < VPAGES_PER_VBLOCK; p++) {
        uint32_t src;
        if (log->page[p] != LOG_NONE) {
            src = log->vblock * VPAGES_PER_VBLOCK + log->page[p];
        }
        else if (dvb < BLOCKS_PER_DIE / 2) {
            src = dvb * VPAGES_PER_VBLOCK + p;
        }
        else {
            continue;
        }
        rc = vfl_read_vpage(src, cbuf, meta);
        if (rc < 0) {
            return -40;
        }
        if (rc == 1) {
            continue;           /* blank pages stay blank */
        }
        if (vfl_program(nvb * VPAGES_PER_VBLOCK + p, cbuf, 0xffff, meta)) {
            return -41;
        }
    }
    if (dvb < BLOCKS_PER_DIE / 2) {
        add_free_vb(dvb, ec_get(lbn));
    }
    add_free_vb(log->vblock, log->ec);
    map_set(lbn, nvb, nec);
    log->vblock = LOG_UNUSED;
    log_init(log);
    put16(&ftl_hdr[H_WL_STATIC], be16(&ftl_hdr[H_WL_STATIC]) + 1);
    put16(&ftl_hdr[H_WL_META], be16(&ftl_hdr[H_WL_META]) + 1);
    ftl_stat_merges++;
    return store_cxt(lbn % FTL_MAP_CHUNKS);
}

/* _DoCopyMerge, for a log whose page i holds logical page i (written in
   order, nothing rewritten): the rest of the data block is copied into the
   log block, which becomes the data block; only the old data block is
   freed. A log filled in order (a file copy) needs no copying at all.
   The copied pages carry no transaction end marker, so a power loss in
   between leaves the log as it was (sudden power off rule). */
static bool log_in_order(const struct ftl_log *log)
{
    unsigned int p;

    if (log->dirty || log->nvalid != log->nfree) {
        return false;
    }
    for (p = 0; p < log->nfree; p++) {
        if (log->page[p] != p || log->valid[p] != 0xffff) {
            return false;
        }
    }
    return true;
}

static int copy_merge(struct ftl_log *log)
{
    unsigned int lbn = log->lbn, p;
    uint16_t dvb = map_get(lbn);
    uint8_t meta[20];
    int rc;

    for (p = log->nfree; p < VPAGES_PER_VBLOCK; p++) {
        rc = vfl_read_vpage(dvb * VPAGES_PER_VBLOCK + p, cbuf, meta);
        if (rc < 0) {
            return -42;
        }
        if (rc == 1) {
            continue;           /* blank pages stay blank */
        }
        memset(meta, 0x0f, sizeof(meta));
        put32(meta, p);
        if (vfl_program(log->vblock * VPAGES_PER_VBLOCK + p, cbuf, 0xffff,
                        meta)) {
            return -43;
        }
    }
    add_free_vb(dvb, ec_get(lbn));
    map_set(lbn, log->vblock, log->ec);
    log->vblock = LOG_UNUSED;
    log_init(log);
    put16(&ftl_hdr[H_WL_STATIC], be16(&ftl_hdr[H_WL_STATIC]) + 1);
    put16(&ftl_hdr[H_WL_META], be16(&ftl_hdr[H_WL_META]) + 1);
    ftl_stat_merges++;
    ftl_stat_copymerges++;
    return store_cxt(lbn % FTL_MAP_CHUNKS);
}

static int merge(struct ftl_log *log)
{
    if (map_get(log->lbn) < BLOCKS_PER_DIE / 2 && log_in_order(log)) {
        return copy_merge(log);
    }
    return simple_merge(log);
}

/* _PrepareLog: the log of 'lbn', assigning one if needed */
static int prepare_log(unsigned int lbn, struct ftl_log **out)
{
    struct ftl_log *log = ftl_find_log(lbn), *victim;
    int i, rc;

    if (log && log->dirty) {
        /* an interrupted write: merge it like the original firmware */
        rc = simple_merge(log);
        if (rc) {
            return rc;
        }
        log = NULL;
    }
    if (!log) {
        /* an assigned but empty log can be reused */
        for (i = 0; i < FTL_LOGS && !log; i++) {
            if (ftl_logs[i].vblock != LOG_UNUSED && ftl_logs[i].nfree == 0 &&
                !ftl_logs[i].dirty) {
                log = &ftl_logs[i];
            }
        }
        if (!log) {
            if (be16(&ftl_hdr[H_NFREE]) <= 3) {
                /* all logs in use: merge the least recently used one */
                victim = NULL;
                for (i = 0; i < FTL_LOGS; i++) {
                    struct ftl_log *l = &ftl_logs[i];
                    if (l->vblock != LOG_UNUSED && l->lbn >= 2 &&
                        (!victim || l->age < victim->age)) {
                        victim = l;
                    }
                }
                if (!victim) {
                    return -50;
                }
                rc = merge(victim);
                if (rc) {
                    return rc;
                }
            }
            for (i = 0; i < FTL_LOGS && !log; i++) {
                if (ftl_logs[i].vblock == LOG_UNUSED) {
                    log = &ftl_logs[i];
                }
            }
            if (!log) {
                return -51;
            }
            rc = get_free_vb(&log->vblock, &log->ec);
            if (rc) {
                log->vblock = LOG_UNUSED;
                return rc;
            }
        }
        log->lbn = lbn;
        log_init(log);
        rc = store_cxt(lbn % FTL_MAP_CHUNKS);
        if (rc) {
            return rc;
        }
    }
    log->age = ++ftl_age_counter;
    *out = log;
    return 0;
}

/* appends one full virtual page (16 sectors) of logical block 'lbn' */
static int write_vpage(unsigned int lbn, unsigned int lpage, const uint8_t *data)
{
    struct ftl_log *log;
    uint8_t meta[20];
    unsigned int off;
    int rc;

    rc = prepare_log(lbn, &log);
    if (rc) {
        return rc;
    }
    if (log->nfree > VPAGES_PER_VBLOCK - 2 + (lpage & 1)) {
        /* the log is full: merge it and start a new one for this lbn */
        rc = merge(log);
        if (rc) {
            return rc;
        }
        rc = get_free_vb(&log->vblock, &log->ec);
        if (rc) {
            log->vblock = LOG_UNUSED;
            return rc;
        }
        log->lbn = lbn;
        log_init(log);
        log->age = ++ftl_age_counter;
        rc = store_cxt(lbn % FTL_MAP_CHUNKS);
        if (rc) {
            return rc;
        }
    }
    off = log->nfree + ((lpage - log->nfree) & 1);  /* same die */

    memset(meta, 0x0f, sizeof(meta));
    put32(meta, lpage);
    meta[4 + 15] = 0x00;                    /* end of transaction */
    rc = vfl_program(log->vblock * VPAGES_PER_VBLOCK + off, data, 0xffff,
                     meta);
    if (rc) {
        return rc;
    }
    if (log->page[lpage] == LOG_NONE) {
        log->nvalid++;
    }
    log->page[lpage] = off;
    log->valid[lpage] = 0xffff;
    log->nfree = off + 1;
    ftl_stat_pages++;
    return 0;
}

/* writes sectors to the flash, read-modify-write for partial pages */
static uint32_t ftl_write_nand(uint32_t sector, uint32_t count,
                               const uint8_t *src)
{
    if (!ftl_ok || !ftl_write_enabled || ftl_write_failed) {
        return 1;
    }
    /* whole pages: the first page of the partition also holds the MBR,
       ftl_write() only lets the partition's own sectors be changed and
       the cache page was read from the flash, so the others are written
       back unchanged */
    if (sector < ftl_write_first - ftl_write_first % SECTORS_PER_VPAGE ||
        sector + count > ftl_write_end) {
        return 2;
    }
    nand_set_active();
    while (count) {
        unsigned int lbn = sector / SECTORS_PER_LBLOCK;
        unsigned int lpage = (sector / SECTORS_PER_VPAGE) % VPAGES_PER_VBLOCK;
        unsigned int sec = sector % SECTORS_PER_VPAGE;
        unsigned int n = MIN(count, SECTORS_PER_VPAGE - sec);
        const uint8_t *data;
        int rc;

        if (n == SECTORS_PER_VPAGE) {
            data = src;
        }
        else {
            if (ftl_read_nand(sector - sec, SECTORS_PER_VPAGE, wbuf)) {
                return wfail(3);
            }
            memcpy(wbuf + sec * SECTOR_SIZE, src, n * SECTOR_SIZE);
            data = wbuf;
        }
        rc = write_vpage(lbn, lpage, data);
        if (rc) {
            return wfail(rc < 0 ? -rc : rc);
        }
        src += n * SECTOR_SIZE;
        sector += n;
        count -= n;
    }
    return 0;
}

/* the size of the disk shown to Rockbox and over USB: up to the end of
   the music partition, so the original firmware's system partition (and
   the hidden area after it) is out of reach of the host */
uint32_t ftl_visible_sectors(void)
{
    return ftl_write_end ? ftl_write_end : ftl_sector_count();
}

/* the FAT32 partition of the MBR is the only writable area */
static void ftl_write_setup(void)
{
    ftl_write_first = ftl_write_end = 0;
    if (ftl_read_nand(0, 1, buf)) {
        return;
    }
    if (buf[0x1fe] != 0x55 || buf[0x1ff] != 0xaa) {
        return;
    }
    uint32_t start = buf[0x1c6] | (buf[0x1c7] << 8) | (buf[0x1c8] << 16) |
                     ((uint32_t)buf[0x1c9] << 24);
    uint32_t size = buf[0x1ca] | (buf[0x1cb] << 8) | (buf[0x1cc] << 16) |
                    ((uint32_t)buf[0x1cd] << 24);
    /* sanity: inside the visible area, before the hidden firmware area */
    if (start == 0 || size == 0 || start + size > 7786496) {
        return;
    }
    ftl_write_first = start;
    ftl_write_end = start + size;
#ifndef BOOTLOADER
    /* the main firmware writes (the music partition only) */
    ftl_write_enabled = true;
    nand_ll_write_enable = true;
#endif
}

/* development: merge the log of 'lbn' (e.g. one with pages that were
   programmed without data and must not be programmed again) */
int ftl_dev_merge(unsigned int lbn)
{
    struct ftl_log *log;
    int rc;

    if (!ftl_ok || !ftl_write_enabled || ftl_write_failed) {
        return -1;
    }
    log = ftl_find_log(lbn);
    if (!log) {
        return -2;
    }
    rc = simple_merge(log);
    if (rc) {
        wfail(rc);
    }
    return rc;
}

void ftl_write_log(void (*logf_fn)(const char *fmt, ...))
{
    extern int nand_ll_last_status;
    logf_fn("ftl write: en %d failed %d err %d nandst %02x range %lu-%lu "
            "pages %lu merges %lu (copy %lu) cxt %lu erases %lu free %d",
            ftl_write_enabled, ftl_write_failed, ftl_write_last_error,
            nand_ll_last_status & 0xff, (unsigned long)ftl_write_first,
            (unsigned long)ftl_write_end, ftl_stat_pages, ftl_stat_merges,
            ftl_stat_copymerges, ftl_stat_cxt, ftl_stat_erases,
            be16(&ftl_hdr[H_NFREE]));
}

#ifdef FTL_SIM
/* host test hooks (utils/s5l8700_dfu/ftlsim) */
void ftl_sim_set_range(uint32_t first, uint32_t end)
{
    ftl_write_first = first;
    ftl_write_end = end;
}

/* checks the invariants the original firmware relies on */
int ftl_sim_check(void (*logf_fn)(const char *fmt, ...))
{
    static uint8_t owner[BLOCKS_PER_DIE / 2];
    int errors = 0, i, used = 0;
    unsigned int n = be16(&ftl_hdr[H_NFREE]), head = be16(&ftl_hdr[H_HEAD]);
    const uint16_t *meta = vfl_counter[0] >= vfl_counter[1] ? vfl_meta[0]
                                                             : vfl_meta[1];
    memset(owner, 0, sizeof(owner));
#define CLAIM(vb, what) do { unsigned int v_ = (vb); \
        if (v_ >= BLOCKS_PER_DIE / 2) { logf_fn("bad vb %u (%s)", v_, what); errors++; } \
        else if (owner[v_]) { logf_fn("vb %u used twice (%s, %d)", v_, what, owner[v_]); errors++; } \
        else owner[v_] = 1; } while (0)
    for (i = 0; i < (int)ftl_lblocks; i++) CLAIM(map_get(i), "map");
    for (i = 0; i < FTL_LOGS; i++) {
        if (ftl_logs[i].vblock == LOG_UNUSED) continue;
        used++;
        CLAIM(ftl_logs[i].vblock, "log");
        if (ftl_logs[i].dirty) { logf_fn("log %d dirty", i); errors++; }
        int p;
        for (p = ftl_logs[i].nfree; p < ftl_logs[i].nfree + 3 && p < 256; p++) {
            vfl_read_spare(ftl_logs[i].vblock * 256 + p, spare);
            if (!spare_empty(spare)) { logf_fn("log %d page %d after commit not blank", i, p); errors++; }
        }
        for (p = 0; p < 256; p++)
            if (ftl_logs[i].page[p] != LOG_NONE && (ftl_logs[i].page[p] & 1) != (p & 1)) {
                logf_fn("log %d page %d parity", i, p); errors++;
            }
    }
    for (i = 0; i < (int)n; i++)
        CLAIM(be16(&ftl_hdr[H_RING + 2 * ((head + i) % RING_SIZE)]), "free");
    for (i = 0; i < 9; i++) {
        unsigned int svb = be16(&ftl_hdr[H_STREAM_VB + 2 * i]);
        unsigned int spg = be16(&ftl_hdr[H_STREAM_PG + 2 * i]);
        CLAIM(svb, "stream");
        if (spg < 255) {
            vfl_read_spare(svb * 256 + spg + 1, spare);
            if (!spare_empty(spare)) { logf_fn("stream %d next page not blank", i); errors++; }
        }
    }
    CLAIM(be32(&ftl_hdr[H_SPARE_VB]), "spare meta");
    /* the stream + spare blocks must be the VFL's meta list */
    for (i = 0; i < FTL_META_BLOCKS; i++)
        if (meta[i] >= BLOCKS_PER_DIE / 2 || !owner[meta[i]]) {
            logf_fn("meta list vb %u not a stream/spare block", meta[i]); errors++;
        }
    if (n + used != 15) { logf_fn("free %u + logs %d != 15", n, used); errors++; }
    logf_fn("check: free %u logs %d usn %08lx errors %d", n, used,
            (unsigned long)be32(&ftl_hdr[H_USN]), errors);
    return errors;
}
#endif

/*  Write cache of a few virtual pages. Rockbox and USB hosts write single
    sectors (FAT, FSInfo, directory entries) and chunks that don't start on
    a page; each page written costs a whole 8 KiB page in the log. Pages are
    written when evicted (least recently used), on ftl_sync() and when the
    storage goes idle (nand_event). Pages of one logical block are always
    written in increasing order, so that a file copy leaves its logs in
    order and they can be merged without copying (copy_merge). */
#define WCACHE_NONE 0xffffffff
#define WCACHE_PAGES 4
unsigned long ftl_write_refused, ftl_write_refused_sector;
unsigned long ftl_stat_wcalls, ftl_stat_wlast;
unsigned long ftl_stat_rerr;
int ftl_stat_rerr_rc;
static struct {
    uint32_t sector;            /* first sector of the page */
    bool dirty;
    unsigned long used;
} wcache[WCACHE_PAGES];
static uint8_t wcache_buf[WCACHE_PAGES][2 * PAGE_SIZE]
    SDRAM_BSS_ATTR __attribute__((aligned(16)));
static unsigned long wcache_clock;

static void cache_init(void)
{
    int i;
    for (i = 0; i < WCACHE_PAGES; i++) {
        wcache[i].sector = WCACHE_NONE;
        wcache[i].dirty = false;
    }
    wcache_ready = true;
}

static int cache_find(uint32_t page)
{
    int i;
    for (i = 0; i < WCACHE_PAGES; i++) {
        if (wcache[i].sector == page) {
            return i;
        }
    }
    return -1;
}

static uint32_t cache_write_one(int i)
{
    uint32_t rc = 0;
    if (wcache[i].sector != WCACHE_NONE && wcache[i].dirty) {
        rc = ftl_write_nand(wcache[i].sector, SECTORS_PER_VPAGE,
                            wcache_buf[i]);
        if (rc) {
            wcache[i].sector = WCACHE_NONE;
        }
    }
    wcache[i].dirty = false;
    return rc;
}

/* writes the dirty pages of the logical block of 'page' that come before
   it (all of them with 'page' = end of the block), lowest first */
static uint32_t cache_write_below(uint32_t page)
{
    uint32_t lblock = page - page % SECTORS_PER_LBLOCK;
    for (;;) {
        int i, low = -1;
        for (i = 0; i < WCACHE_PAGES; i++) {
            if (wcache[i].dirty && wcache[i].sector >= lblock &&
                wcache[i].sector < page &&
                (low < 0 || wcache[i].sector < wcache[low].sector)) {
                low = i;
            }
        }
        if (low < 0) {
            return 0;
        }
        uint32_t rc = cache_write_one(low);
        if (rc) {
            return rc;
        }
    }
}

/* writes a cached page, after the ones before it in its logical block */
static uint32_t cache_write(int i)
{
    uint32_t rc = cache_write_below(wcache[i].sector);
    return rc ? rc : cache_write_one(i);
}

static uint32_t cache_flush(void)
{
    uint32_t rc = 0;
    int i;
    for (i = 0; i < WCACHE_PAGES && !rc; i++) {
        if (wcache[i].dirty) {
            rc = cache_write(i);
        }
    }
    return rc;
}

uint32_t ftl_read(uint32_t sector, uint32_t count, void *buffer)
{
    uint32_t rc;
    int i;
    FTL_LOCK();
    rc = ftl_read_nand(sector, count, buffer);
    for (i = 0; !rc && wcache_ready && i < WCACHE_PAGES; i++) {
        uint32_t cs = wcache[i].sector;
        if (cs != WCACHE_NONE && wcache[i].dirty &&
            sector < cs + SECTORS_PER_VPAGE && sector + count > cs) {
            uint32_t first = MAX(sector, cs);
            uint32_t end = MIN(sector + count, cs + SECTORS_PER_VPAGE);
            memcpy((uint8_t *)buffer + (first - sector) * SECTOR_SIZE,
                   wcache_buf[i] + (first - cs) * SECTOR_SIZE,
                   (end - first) * SECTOR_SIZE);
        }
    }
    FTL_UNLOCK();
    return rc;
}

uint32_t ftl_write(uint32_t sector, uint32_t count, const void *buffer)
{
    const uint8_t *src = buffer;
    uint32_t rc = 0;

    FTL_LOCK();
    ftl_stat_wcalls++;
    ftl_stat_wlast = sector;
    if (!ftl_ok || !ftl_write_enabled || ftl_write_failed ||
        sector < ftl_write_first || sector + count > ftl_write_end) {
        ftl_write_refused++;
        ftl_write_refused_sector = sector;
        FTL_UNLOCK();
        return 1;
    }
    if (!wcache_ready) {
        cache_init();
    }
    while (count && !rc) {
        uint32_t page = sector - sector % SECTORS_PER_VPAGE;
        uint32_t sec = sector - page;
        uint32_t n = MIN(count, SECTORS_PER_VPAGE - sec);
        int i = cache_find(page);

        if (i < 0 && n == SECTORS_PER_VPAGE) {
            /* a whole page: straight to the flash, after the cached pages
               before it in its logical block */
            rc = cache_write_below(page);
            if (!rc) {
                rc = ftl_write_nand(page, SECTORS_PER_VPAGE, src);
            }
            src += n * SECTOR_SIZE;
            sector += n;
            count -= n;
            continue;
        }
        if (i < 0) {
            /* a free slot, or the least recently used page */
            int j;
            for (i = 0, j = 1; j < WCACHE_PAGES; j++) {
                if (wcache[i].sector == WCACHE_NONE) {
                    break;
                }
                if (wcache[j].sector == WCACHE_NONE ||
                    wcache[j].used < wcache[i].used) {
                    i = j;
                }
            }
            rc = cache_write(i);
            if (rc) {
                break;
            }
            wcache[i].sector = WCACHE_NONE;
            if (n < SECTORS_PER_VPAGE) {
                rc = ftl_read_nand(page, SECTORS_PER_VPAGE, wcache_buf[i]);
                if (rc) {
                    ftl_stat_rerr++;
                    ftl_stat_rerr_rc = rc;
                    rc = 3;
                    break;
                }
            }
            wcache[i].sector = page;
        }
        memcpy(wcache_buf[i] + sec * SECTOR_SIZE, src, n * SECTOR_SIZE);
        wcache[i].dirty = true;
        wcache[i].used = ++wcache_clock;
        src += n * SECTOR_SIZE;
        sector += n;
        count -= n;
    }
    FTL_UNLOCK();
    return rc;
}

uint32_t ftl_sync(void)
{
    uint32_t rc = 0;
    FTL_LOCK();
    if (wcache_ready) {
        rc = cache_flush();
    }
    FTL_UNLOCK();
    return rc;
}

/* called by the storage thread when the NAND has been idle for a while */
void ftl_idle(void)
{
    int i;
    for (i = 0; wcache_ready && i < WCACHE_PAGES; i++) {
        if (wcache[i].dirty) {
            ftl_sync();
            break;
        }
    }
}
