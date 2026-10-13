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
#include <string.h>

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "s5l87xx.h"
#include "nand-target.h"
#include "storage.h"
#include "sdram-target.h"
#include <stdbool.h>

/*  Driver for the S5L8700 flash memory controller for low-level access to the
    NAND flash of the Samsung YP-S3.

    The YP-S3 seems to use the pins P6.5 and P6.6 as chip selects in GPIO mode
    instead of using the regular pins P6.3 and P6.4.
*/

#define FMSTAT_RBB              (1<<0)
#define FMSTAT_RBBDone          (1<<1)
#define FMSTAT_CMDDone          (1<<2)
#define FMSTAT_AddrDone         (1<<3)
#define FMSTAT_TransDone        (1<<4)
#define FMSTAT_WFIFO_HEMPTY     (1<<5)
#define FMSTAT_RFIFO_HFULL      (1<<6)
#define FMSTAT_WFIFO_EMPTY      (1<<8)
#define FMSTAT_RFIFO_FULL       (1<<9)
#define FMSTAT_EndECC           (1<<10)

#define FMCTRL1_DoTransAddr     (1<<0)
#define FMCTRL1_DoReadData      (1<<1)
#define FMCTRL1_DoWriteData     (1<<2)
#define FMCTRL1_WriteREQSEL     (1<<4)
#define FMCTRL1_ClearSyndPtr    (1<<5)
#define FMCTRL1_ClearWFIFO      (1<<6)
#define FMCTRL1_ClearRFIFO      (1<<7)
#define FMCTRL1_ParityPtr       (1<<8)
#define FMCTRL1_SyndPtr         (1<<9)

#define NAND_CMD_READ           0x00
#define NAND_CMD_READ2          0x30
#define NAND_CMD_GET_STATUS     0x70
#define NAND_CMD_READ_ID        0x90
#define NAND_CMD_RESET          0xFF

#define NAND_STATUS_READY       0x40

/* rough busy-wait limit, a few tens of milliseconds at 200 MHz */
#define NAND_TIMEOUT            1000000

/* How the address cycles are handed to the controller:
   1 = one byte per FMADDRn register (S5L8700, assumed),
   0 = packed into FMADDR0/FMADDR1 like the S5L8701 Nano 2G driver.
   Reading always page 0 / column 0 showed the packed form doesn't work
   here. Exposed so the NAND probe can toggle it. */
int nand_ll_addr_swap = 1;

static void nand_chip_select(int bank)
{
    unsigned int select;

    select = (1 << bank);
    FMCTRL0 = 0x1821 | ((select & 3) << 1);
    PDAT6 = (PDAT6 & ~0x60) | ((~select & 0xC) << 3);
}

static int nand_wait_status(unsigned int flag)
{
    int timeout = NAND_TIMEOUT;
    while ((FMCSTAT & flag) == 0) {
        if (--timeout == 0) {
            return -1;
        }
    }
    FMCSTAT = flag;
    return 0;
}

static int nand_send_cmd(unsigned int cmd)
{
    FMCMD = cmd;
    return nand_wait_status(FMSTAT_CMDDone);
}

/* reads 1 to 4 bytes from the chip, returned in the low bytes of the word
   with the first byte in the least significant position */
static int nand_read_word(unsigned int count, unsigned int *word)
{
    FMDNUM = count - 1;
    FMCTRL1 = FMCTRL1_DoReadData;
    if (nand_wait_status(FMSTAT_TransDone)) {
        FMCTRL1 = FMCTRL1_ClearRFIFO;
        return -1;
    }
    *word = FMFIFO;
    FMCTRL1 = FMCTRL1_ClearRFIFO;
    return 0;
}

static int nand_wait_ready(void)
{
    unsigned int status;
    int timeout = NAND_TIMEOUT / 100;

    if (nand_send_cmd(NAND_CMD_GET_STATUS)) {
        return -1;
    }
    do {
        /* the chip repeats the status byte, so read a whole word and
           don't care in which byte lane it ends up */
        if (nand_read_word(4, &status)) {
            return -1;
        }
        if (--timeout == 0) {
            return -1;
        }
    } while ((status & (NAND_STATUS_READY * 0x01010101)) == 0);

    return 0;
}

void nand_ll_init(void)
{
    /* enable flash memory controller */
    PWRCON &= ~(1 << 1);

    /* P2.X is SMC I/O */
    PCON2 = 0x55555555;
    /* P4.1 = CLE, P4.4 = nWR, P4.5 = nRD */
    PCON4 = (PCON4 & ~0x00FF00F0) | 0x00550050;
    /* P6.0 = nf_rbn, P6.1 = smc_ce0, P6.2 = smc_ce1,
       P6.5 = smc_ce2 (as GPIO), P6.6 = smc_ce3 (as GPIO), P6.7 = ALE */
    PCON6 = (PCON6 & ~0xFFF00FFF) | 0x51100555;
    PDAT6 |= 0x60;
}

int nand_ll_reset(int bank)
{
    nand_chip_select(bank);
    FMCTRL1 = FMCTRL1_ClearRFIFO | FMCTRL1_ClearWFIFO;
    if (nand_send_cmd(NAND_CMD_RESET)) {
        return -1;
    }
    return nand_wait_ready();
}

unsigned int nand_ll_read_id(int bank)
{
    unsigned int nand_id;

    nand_chip_select(bank);
    
    /* send "read id" command */
    FMCMD = NAND_CMD_READ_ID;
    while ((FMCSTAT & FMSTAT_CMDDone) == 0);
    FMCSTAT = FMSTAT_CMDDone;
    
    /* transfer address */
    FMANUM = 0;
    FMADDR0 = 0;
    FMCTRL1 = FMCTRL1_DoTransAddr;
    while ((FMCSTAT & FMSTAT_AddrDone) == 0);
    FMCSTAT = FMSTAT_AddrDone;
    
    /* read back data */
    FMDNUM = 3;
    FMCTRL1 = FMCTRL1_DoReadData;
    while ((FMCSTAT & FMSTAT_TransDone) == 0);
    FMCSTAT = FMSTAT_TransDone;
    nand_id = FMFIFO;
    
    /* clear read FIFO */
    FMCTRL1 = FMCTRL1_ClearRFIFO;
    
#ifdef ROCKBOX_BIG_ENDIAN
    /* the first byte arrives in the most significant lane; return it in the
       least significant byte like the Nano 2G driver does (maker in bits 0-7) */
    nand_id = swap32(nand_id);
#endif
    return nand_id;    
}

/*  DMA transfer of the data phase, set up like the original firmware does
    (DMA channel 1, FMC data request). The FIFO hands out words with the
    first byte in the most significant lane, which is exactly the memory
    order of a big-endian word, so no swapping is needed. */
bool nand_ll_use_dma = false;
static unsigned char dma_bounce[4096] SDRAM_BSS_ATTR __attribute__((aligned(16)));

static int nand_read_dma(int bank, unsigned char *buf, unsigned int len)
{
    unsigned char *dst = buf;
    long timeout = 1000000;

    if (((unsigned int)buf & 15) || len > sizeof(dma_bounce)) {
        dst = dma_bounce;
        if (len > sizeof(dma_bounce)) {
            return -1;
        }
    }

    /* data phase with DMA requests enabled (FMCTRL0 bit 7) */
    FMCTRL0 = 0x18a1 | (((1 << bank) & 3) << 1);

    DMACOM1 = 7;
    DMACON1 = 0x40880000;
    /* the ARM940T only has index based cache operations, so the range
       versions can't be used */
    commit_discard_dcache();
    DMABASE1 = (unsigned int)dst;
    DMATCNT1 = (len >> 3) - 1;
    FMDNUM = len - 1;
    FMCTRL1 = FMCTRL1_DoReadData | 0x10;
    DMACOM1 = 4;
    while (!(DMAALLST & 0x10)) {
        if (--timeout == 0) {
            DMACOM1 = 7;
            nand_chip_select(bank);
            return -1;
        }
    }
    DMACOM1 = 7;
    FMCTRL1 = FMCTRL1_ClearRFIFO;
    nand_chip_select(bank);
    commit_discard_dcache();
    if (dst != buf) {
        memcpy(buf, dst, len);
    }
    return 0;
}

/*  Reads 'len' raw bytes (no ECC) of a page, starting at column 'col'.
    'len' should be a multiple of 4.
    This assumes a large page device (2 column + 3 row address cycles).
    It is slow (programmed I/O, 4 bytes per transfer) and only intended
    for probing the flash layout. */
int nand_ll_read_raw(int bank, unsigned int page, unsigned int col,
                     unsigned char *buf, unsigned int len)
{
    unsigned int i, word, count;

    nand_chip_select(bank);
    FMCTRL1 = FMCTRL1_ClearRFIFO | FMCTRL1_ClearWFIFO;

    if (nand_send_cmd(NAND_CMD_READ)) {
        return -1;
    }

    FMANUM = 4;
    if (nand_ll_addr_swap) {
        FMADDR0 = col & 0xFF;
        FMADDR1 = (col >> 8) & 0xFF;
        FMADDR2 = page & 0xFF;
        FMADDR3 = (page >> 8) & 0xFF;
        FMADDR4 = (page >> 16) & 0xFF;
    } else {
        FMADDR0 = (page << 16) | (col & 0xFFFF);
        FMADDR1 = (page >> 16) & 0xFF;
    }
    FMCTRL1 = FMCTRL1_DoTransAddr;
    if (nand_wait_status(FMSTAT_AddrDone)) {
        return -2;
    }

    if (nand_send_cmd(NAND_CMD_READ2)) {
        return -3;
    }
    if (nand_wait_ready()) {
        return -4;
    }
    /* back to data output mode after polling the status */
    if (nand_send_cmd(NAND_CMD_READ)) {
        return -5;
    }

    if (nand_ll_use_dma && len >= 16 && (len % 16) == 0) {
        return nand_read_dma(bank, buf, len) ? -7 : 0;
    }

    for (i = 0; i < len; i += count) {
        count = (len - i) < 4 ? (len - i) : 4;
        if (nand_read_word(count, &word)) {
            return -6;
        }
#ifdef ROCKBOX_BIG_ENDIAN
        /* first byte in the most significant lane (confirmed by the ID) */
        word = swap32(word);
#endif
        buf[i] = word & 0xFF;
        if (count > 1) buf[i + 1] = (word >> 8) & 0xFF;
        if (count > 2) buf[i + 2] = (word >> 16) & 0xFF;
        if (count > 3) buf[i + 3] = (word >> 24) & 0xFF;
    }

    return 0;
}

static long nand_last_activity_value = -1;

void nand_set_active(void)
{
    nand_last_activity_value = current_tick;
}

long nand_last_activity(void)
{
    return nand_last_activity_value;
}

void nand_power_down(void)
{
    /* TODO: the NAND has no separate power control that we know of */
}

int nand_event(long id, intptr_t data)
{
    int rc = 0;

    if (LIKELY(id == Q_STORAGE_TICK)) {
        if (TIME_BEFORE(current_tick, nand_last_activity_value + HZ / 5)) {
            STG_EVENT_ASSERT_ACTIVE(STORAGE_NAND);
        }
    }
    else if (id == Q_STORAGE_SLEEPNOW) {
        nand_power_down();
    }
    else {
        rc = storage_event_default_handler(id, data, nand_last_activity_value,
                                           STORAGE_NAND);
    }
    return rc;
}

/*  Hardware ECC, used the way the original firmware's FIL does it:
    each 512 byte sector has 9 ECC bytes in the spare area (from 0x28), and
    the 20 metadata bytes at spare 12-31 have 9 ECC bytes at spare 2.
    The engine corrects the buffer in place. Returns <0 if uncorrectable,
    else the number of corrected errors (as reported by the engine). */

#define ECC_SRCPND          (*(REG32_PTR_T)(0x39C00000))
#define ECC_INTPND          (*(REG32_PTR_T)(0x39C00010))
#define ECC_SIZE_SEL        (*(REG32_PTR_T)(ECC_BASE + 0x40))

static unsigned char ecc_bytes[12] __attribute__((aligned(4)));

static bool ecc_bytes_erased(const unsigned char *ecc)
{
    int i, zeros = 0;
    for (i = 0; i < 9; i++) {
        zeros += 8 - __builtin_popcount(ecc[i]);
    }
    return zeros < 4;   /* like the OF: an erased (or nearly) ECC field */
}

int nand_ll_ecc_correct(void *data, const unsigned char *ecc, bool sector)
{
    unsigned int result;
    int timeout = 100000;

    if (ecc_bytes_erased(ecc)) {
        return 0;
    }
    memcpy(ecc_bytes, ecc, 9);
    commit_dcache();

    ECC_SRCPND = (1 << 19);
    ECC_INTPND = (1 << 19);
    ECC_DATA_PTR = (unsigned int)data;
    ECC_SPARE_PTR = (unsigned int)ecc_bytes;
    ECC_CTRL = ECCCTRL_STARTDECODING;
    ECC_SIZE_SEL = sector ? 1 : 0;     /* same order as the original firmware */
    /* the pending bit may still be set from the previous decode for a
       moment, so give the engine some time first (~25 us at 200 MHz) */
    for (volatile int i = 0; i < 1000; i++);
    while (!(ECC_SRCPND & (1 << 19)) && --timeout);

    commit_discard_dcache();
    result = ECC_RESULT;
    if (result & 1) {
        return -1;
    }
    return (result & 0xf) >> 1;
}

/*  ECC encoding, for writing: like the original firmware's page program,
    CTRL = 2 starts encoding 'data' (a 512 byte sector or the 20 metadata
    bytes, chosen with SIZE_SEL) and the engine stores the 9 ECC bytes at
    the spare pointer. Validated against ECC bytes already on the flash
    (ftl_ecc_encode_selftest) before it is used. */
static unsigned char ecc_out[16] __attribute__((aligned(16)));

int nand_ll_ecc_encode(const void *data, unsigned char *ecc, int size_sel)
{
    int timeout = 100000;

    commit_dcache();
    ECC_SRCPND = (1 << 19);
    ECC_INTPND = (1 << 19);
    ECC_DATA_PTR = (unsigned int)data;
    ECC_SPARE_PTR = (unsigned int)ecc_out;
    ECC_CTRL = 2;
    ECC_SIZE_SEL = size_sel;
    for (volatile int i = 0; i < 1000; i++);
    while (!(ECC_SRCPND & (1 << 19)) && --timeout);
    commit_discard_dcache();
    memcpy(ecc, ecc_out, 9);
    return timeout ? 0 : -1;
}
