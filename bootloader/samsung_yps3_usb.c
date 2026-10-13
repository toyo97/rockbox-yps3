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

/*  Minimal polled USB device for the Samsung YP-S3 test bootloader, used to
    read the NAND flash from a PC (utils/s5l8700_dfu/yps3_nanddump.py).

    The S5L8700 has the same USB 2.0 function controller as the Samsung
    S3C2443/S3C2416 (Linux: drivers/usb/gadget/udc/s3c-hsudc.c, removed in
    6.3). The boot ROM leaves it enumerated as its DFU device (0419:0145)
    when it starts us, so we keep that identity and only answer control
    requests on endpoint 0. Standard requests are handled too, in case the
    host resets and re-enumerates the device.

    Everything is read-only: there is no request that writes to the flash.
*/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "s5l87xx.h"
#include "nand-target.h"
#include "samsung_yps3_usb.h"

/* register bits, from s3c-hsudc.c */
#define SSR_RESET           (1 << 0)
#define EP0SR_RX_SUCCESS    (1 << 0)
#define EP0SR_TX_SUCCESS    (1 << 1)
#define EP0SR_STALL         (1 << 4)
#define EP0CR_STALL         (1 << 1)

#define EP0_MAX_PACKET      64

/* vendor requests (bmRequestType 0xC0) */
#define REQ_INFO            0x50    /* -> text */
#define REQ_READ_PAGE       0x51    /* wValue = page bits 0-15, wIndex:
                                       bits 0-3 page bits 16-19,
                                       bits 4-5 bank,
                                       bits 8-15 column / 128 */
#define REQ_READ_IDS        0x52    /* -> 4 x u32 (little endian) */
#define REQ_READ_MEM        0x53    /* wValue | wIndex << 16 = address */
#define REQ_EXIT            0x5F    /* leave dump mode */

#define NAND_PAGE_TOTAL     (4096 + 128)

static unsigned char buf[NAND_PAGE_TOTAL] __attribute__((aligned(4)));
static const unsigned int *nand_ids;

/* -1 = unknown, 0 = first byte in the low half of a FIFO halfword,
   1 = first byte in the high half. Detected from setup packets. */
static int lanes_swapped = -1;

unsigned int usbdump_requests;
int usbdump_sdram_mb = -1;
static int exit_requested;

static const unsigned char device_desc[18] = {
    18, 1,              /* bLength, DEVICE */
    0x00, 0x02,         /* bcdUSB 2.00 */
    0xFF, 0xFF, 0xFF,   /* vendor class */
    EP0_MAX_PACKET,
    0x19, 0x04,         /* idVendor 0x0419 (same as the boot ROM DFU) */
    0x45, 0x01,         /* idProduct 0x0145 */
    0x99, 0x09,         /* bcdDevice 0x0999 marks this program */
    0, 0, 0,            /* no strings */
    1                   /* one configuration */
};

static const unsigned char config_desc[18] = {
    9, 2, 18, 0,        /* bLength, CONFIGURATION, wTotalLength */
    1, 1, 0,            /* one interface, value 1, no string */
    0x80, 50,           /* bus powered, 100 mA */
    9, 4, 0, 0,         /* bLength, INTERFACE, number 0, alt 0 */
    0,                  /* no endpoints besides ep0 */
    0xFF, 0xFF, 0xFF,   /* vendor class */
    0
};

static bool is_request_type(unsigned int v)
{
    switch (v) {
    case 0x00: case 0x01: case 0x02: case 0x80: case 0x81: case 0x82:
    case 0x21: case 0xA1: case 0x40: case 0xC0: case 0x41: case 0xC1:
        return true;
    default:
        return false;
    }
}

static void unpack_halfword(unsigned int h, unsigned char *p)
{
    if (lanes_swapped > 0) {
        p[0] = (h >> 8) & 0xFF;
        p[1] = h & 0xFF;
    } else {
        p[0] = h & 0xFF;
        p[1] = (h >> 8) & 0xFF;
    }
}

static unsigned int pack_halfword(unsigned char a, unsigned char b)
{
    return (lanes_swapped > 0) ? ((a << 8) | b) : (a | (b << 8));
}

static void ep0_stall(void)
{
    USB_EP0CR |= EP0CR_STALL;
}

static int wait_tx_done(void)
{
    long timeout = current_tick + HZ;

    while (!(USB_EP0SR & EP0SR_TX_SUCCESS)) {
        if (USB_EP0SR & EP0SR_STALL) {
            return -1;
        }
        /* a new setup packet means the host gave up on this transfer */
        if (USB_EP0SR & EP0SR_RX_SUCCESS) {
            return -1;
        }
        if (TIME_AFTER(current_tick, timeout)) {
            return -1;
        }
    }
    USB_EP0SR = EP0SR_TX_SUCCESS;
    return 0;
}

/* data stage of an IN control transfer */
static void ep0_send(const unsigned char *data, unsigned int len,
                     unsigned int wlength)
{
    unsigned int sent = 0, n, i;

    if (len > wlength) {
        len = wlength;
    }

    USB_IR = 0;
    for (;;) {
        n = len - sent;
        if (n > EP0_MAX_PACKET) {
            n = EP0_MAX_PACKET;
        }
        USB_BWCR = n;
        for (i = 0; i < n; i += 2) {
            unsigned char b = (i + 1 < n) ? data[sent + i + 1] : 0;
            USB_EP0BR = pack_halfword(data[sent + i], b);
        }
        sent += n;
        if (wait_tx_done()) {
            return;
        }
        if (n < EP0_MAX_PACKET) {
            return;     /* a short packet ends the data stage */
        }
        if (sent == len) {
            if (len < wlength) {
                /* host expects more: end with a zero length packet */
                USB_BWCR = 0;
                wait_tx_done();
            }
            return;
        }
    }
}

static void handle_setup(void)
{
    unsigned int h[4] = {0, 0, 0, 0};
    unsigned char s[8];
    unsigned int count, i;
    unsigned int type, req, value, index, length;

    count = USB_BRCR;
    for (i = 0; i < count; i++) {
        unsigned int v = USB_EP0BR & 0xFFFF;
        if (i < 4) {
            h[i] = v;
        }
    }
    USB_EP0SR = EP0SR_RX_SUCCESS;

    /* work out the byte order of the FIFO from bmRequestType */
    if (is_request_type(h[0] & 0xFF) && !is_request_type(h[0] >> 8)) {
        lanes_swapped = 0;
    }
    else if (is_request_type(h[0] >> 8) && !is_request_type(h[0] & 0xFF)) {
        lanes_swapped = 1;
    }
    for (i = 0; i < 4; i++) {
        unpack_halfword(h[i], &s[2 * i]);
    }

    type = s[0];
    req = s[1];
    value = s[2] | (s[3] << 8);
    index = s[4] | (s[5] << 8);
    length = s[6] | (s[7] << 8);

    usbdump_requests++;

    if (type == 0xC0) {
        switch (req) {
        case REQ_INFO: {
            int n = snprintf((char *)buf, sizeof(buf),
                             "YPS3DUMP 1 lanes=%d reqs=%u sdram=%dMB",
                             lanes_swapped, usbdump_requests, usbdump_sdram_mb);
            ep0_send(buf, n, length);
            return;
        }
        case REQ_READ_PAGE: {
            unsigned int page = value | ((index & 0xF) << 16);
            int bank = (index >> 4) & 3;
            unsigned int col = ((index >> 8) & 0xFF) * 128;
            unsigned int n = (length + 3) & ~3;
            if (col + n > sizeof(buf)) {
                n = (col < sizeof(buf)) ? sizeof(buf) - col : 0;
            }
            if (n == 0 || nand_ll_read_raw(bank, page, col, buf, n)) {
                ep0_stall();
                return;
            }
            ep0_send(buf, n, length);
            return;
        }
        case REQ_READ_IDS:
            for (i = 0; i < 4; i++) {
                buf[4 * i + 0] = nand_ids[i] & 0xFF;
                buf[4 * i + 1] = (nand_ids[i] >> 8) & 0xFF;
                buf[4 * i + 2] = (nand_ids[i] >> 16) & 0xFF;
                buf[4 * i + 3] = (nand_ids[i] >> 24) & 0xFF;
            }
            ep0_send(buf, 16, length);
            return;
        case REQ_READ_MEM:
            ep0_send((const unsigned char *)(value | (index << 16)),
                     length, length);
            return;
        case REQ_EXIT:
            exit_requested = 1;
            if (length) {
                ep0_send(buf, 0, length);
            }
            return;
        }
        ep0_stall();
        return;
    }

    if ((type & 0x60) == 0) {   /* standard request */
        switch (req) {
        case 0x00:  /* GET_STATUS */
            buf[0] = buf[1] = 0;
            ep0_send(buf, 2, length);
            return;
        case 0x01:  /* CLEAR_FEATURE */
        case 0x03:  /* SET_FEATURE */
        case 0x05:  /* SET_ADDRESS, done by the hardware */
        case 0x09:  /* SET_CONFIGURATION */
        case 0x0B:  /* SET_INTERFACE */
            return;
        case 0x06:  /* GET_DESCRIPTOR */
            switch (value >> 8) {
            case 1:
                ep0_send(device_desc, sizeof(device_desc), length);
                return;
            case 2:
                ep0_send(config_desc, sizeof(config_desc), length);
                return;
            }
            break;
        case 0x08:  /* GET_CONFIGURATION */
            buf[0] = 1;
            ep0_send(buf, 1, length);
            return;
        case 0x0A:  /* GET_INTERFACE */
            buf[0] = 0;
            ep0_send(buf, 1, length);
            return;
        }
    }
    ep0_stall();
}

void usbdump_init(const unsigned int *ids)
{
    nand_ids = ids;
}

int usbdump_poll(void)
{
    unsigned int ep0sr;

    if (USB_SSR & SSR_RESET) {
        /* bus reset: back to a sane default state, like s3c-hsudc does */
        USB_SSR = SSR_RESET;
        USB_EDR = 0xAA;
        USB_EIER = 1;
        USB_TR = 0;
        USB_EP0CR = 0;
        USB_IR = 0;
        USB_MPR = EP0_MAX_PACKET;
    }

    USB_IR = 0;
    ep0sr = USB_EP0SR;
    if (ep0sr & EP0SR_STALL) {
        USB_EP0CR &= ~EP0CR_STALL;
        USB_EP0SR = EP0SR_STALL;
    }
    if (ep0sr & EP0SR_TX_SUCCESS) {
        USB_EP0SR = EP0SR_TX_SUCCESS;
    }
    if (ep0sr & EP0SR_RX_SUCCESS) {
        handle_setup();
        return 1;
    }
    return 0;
}

int usbdump_exit_requested(void)
{
    int r = exit_requested;
    exit_requested = 0;
    return r;
}
