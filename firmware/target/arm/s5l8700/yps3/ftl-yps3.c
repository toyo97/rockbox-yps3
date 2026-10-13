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
#include "config.h"
#include "system.h"
#include "panic.h"
#include "nand-target.h"
#include "ftl-target.h"
#include "sdram-target.h"

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

struct ftl_log {
    uint16_t vblock;
    uint16_t lbn;
    uint8_t page[VPAGES_PER_VBLOCK];        /* log page of each page */
    uint16_t valid[VPAGES_PER_VBLOCK];      /* sectors held by the log */
};
static struct ftl_log ftl_logs[FTL_LOGS] SDRAM_BSS_ATTR;
static int ftl_nlogs;

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

static int ftl_open_log(struct ftl_log *log)
{
    unsigned int p, s;

    memset(log->page, 0xff, sizeof(log->page));
    memset(log->valid, 0, sizeof(log->valid));

    for (p = 0; p < VPAGES_PER_VBLOCK; p++) {
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

    /* the context fields we use are all in the first 0x200 bytes */
    if (vfl_read(best_block * VPAGES_PER_VBLOCK + best_page, 0, buf, 0x200)) {
        return -11;
    }

    uint16_t map_block[FTL_MAP_CHUNKS], map_page[FTL_MAP_CHUNKS];
    for (i = 0; i < FTL_MAP_CHUNKS; i++) {
        map_block[i] = be16(&buf[0xd0 + 2 * i]);
        map_page[i] = be16(&buf[0x154 + 2 * i]);
    }

    ftl_nlogs = 0;
    for (i = 0; i < FTL_LOGS; i++) {
        uint16_t vb = be16(&buf[0x70 + 2 * i]);
        uint16_t lbn = be16(&buf[0x88 + 2 * i]);
        if (vb == 0xffff) {
            continue;
        }
        ftl_logs[ftl_nlogs].vblock = vb;
        ftl_logs[ftl_nlogs].lbn = lbn;
        ftl_nlogs++;
    }

    for (i = 0; i < FTL_MAP_CHUNKS; i++) {
        if (vfl_read(map_block[i] * VPAGES_PER_VBLOCK + map_page[i], 0x200,
                     buf, FTL_MAP_ENTRIES * 2)) {
            return -12;
        }
        unsigned int j;
        for (j = 0; j < FTL_MAP_ENTRIES; j++) {
            ftl_map[i][j] = be16(&buf[2 * j]);
        }
    }

    for (i = 0; i < (unsigned int)ftl_nlogs; i++) {
        if (ftl_open_log(&ftl_logs[i])) {
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

static int ftl_init_rc;

uint32_t ftl_init(void)
{
    int rc;

    nand_ll_init();
    nand_ll_reset(0);

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
    logf_fn("ecc test: clean %d corrected %d failed %d changed %d",
            clean, corrected, failed, changed);
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

static const struct ftl_log *ftl_find_log(unsigned int lbn)
{
    int i;
    for (i = 0; i < ftl_nlogs; i++) {
        if (ftl_logs[i].lbn == lbn) {
            return &ftl_logs[i];
        }
    }
    return NULL;
}

uint32_t ftl_read(uint32_t sector, uint32_t count, void *buffer)
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

        if (log && log->page[vpage] != 0xff &&
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
                   !(log && log->page[vpage] != 0xff &&
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

uint32_t ftl_write(uint32_t sector, uint32_t count, const void *buffer)
{
    (void)sector;
    (void)count;
    (void)buffer;
    return 1;   /* read only for now */
}

uint32_t ftl_sync(void)
{
    return 0;
}
