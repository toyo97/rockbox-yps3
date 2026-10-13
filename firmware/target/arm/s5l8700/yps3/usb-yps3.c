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

/*  USB device driver for the S5L8700's USB 2.0 function controller, the
    same core as the S3C2443/S3C2416 "HS UDC" (Linux: s3c-hsudc.c).

    * Programmed I/O through 16 bit FIFO registers. The SoC runs big endian:
      the first byte of a packet is in bits 15:8 of a FIFO halfword
      (measured with the test bootloader, bootloader/samsung_yps3_usb.c).
    * Endpoint directions are fixed through EDR: odd endpoints IN, even OUT.
    * The controller answers SET_ADDRESS and the status stage of control
      transfers by itself, as with the test bootloader: EP0 status
      transfers are completed in software. */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "s5l87xx.h"
#include "usb.h"
#include "usb_drv.h"
#include "usb_core.h"
#include "usb_ch9.h"
#include "power.h"
#include <string.h>
#include <stdio.h>

#if defined(HAVE_USBSTACK) && !defined(BOOTLOADER)

/* register bits, from s3c-hsudc.c */
#define SSR_RESET           (1 << 0)
#define SSR_SUSPEND         (1 << 1)
#define SSR_RESUME          (1 << 2)
#define SSR_SDE             (1 << 3)
#define SSR_HSP             (1 << 4)
#define SSR_VBUSON          (1 << 8)
#define SSR_ERR             0xff80
#define SSR_ANY             (SSR_RESET | SSR_SUSPEND | SSR_RESUME | SSR_SDE | \
                             SSR_VBUSON | SSR_ERR)

#define SCR_RST_EN          (1 << 0)
#define SCR_SUS_EN          (1 << 1)
#define SCR_RRD_EN          (1 << 5)
#define SCR_DTZIEN_EN       (1 << 14)

#define EP0SR_RX_SUCCESS    (1 << 0)
#define EP0SR_TX_SUCCESS    (1 << 1)
#define EP0SR_STALL         (1 << 4)
#define EP0SR_LWO           (1 << 6)
#define EP0CR_STALL         (1 << 1)

#define ESR_RX_SUCCESS      (1 << 0)
#define ESR_TX_SUCCESS      (1 << 1)
#define ESR_PSIF_MASK       (3 << 2)
#define ESR_PSIF_TWO        (2 << 2)
#define ESR_LWO             (1 << 4)
#define ESR_STALL           (1 << 5)
#define ESR_FLUSH           (1 << 6)

#define ECR_STALL           (1 << 1)
#define ECR_FLUSH           (1 << 6)

#define USB_PHY_REG(o)      (*(REG32_PTR_T)(0x3C400000 + (o)))
#define EP_FIRST_IN         1
#define EP_FIFO(n)          (*(REG32_PTR_T)(USB_FC_BASE + 0x60 + 4 * (n)))

#define EP0_MPS             64

struct ep_state {
    unsigned char *buf;
    int len;            /* requested length */
    int cnt;            /* bytes done */
    bool busy;
    bool zlp;           /* IN: end with a zero length packet */
    bool block;
    int result;
    struct semaphore complete;
};

/* ep_states[n][DIR_OUT/DIR_IN]; EP0 uses both */
static struct ep_state ep_states[USB_NUM_ENDPOINTS][2];
static uint16_t ep0_wlength;
static bool high_speed;

/* shown on the debug screen */
unsigned long usb_dbg_irqs, usb_dbg_resets, usb_dbg_setups, usb_dbg_inits;
unsigned long usb_dbg_last_ssr, usb_dbg_last_ep0sr;
int usb_dbg_variant;

/* last events, for the debug screen: short tags with a value */
#define USB_DBG_EVENTS 24
char usb_dbg_ev[USB_DBG_EVENTS][12];
int usb_dbg_ev_n;
static void dbg_ev(const char *tag, unsigned long v)
{
    char *e = usb_dbg_ev[usb_dbg_ev_n++ % USB_DBG_EVENTS];
    snprintf(e, sizeof(usb_dbg_ev[0]), "%s%lx", tag, v);
}

struct usb_drv_ep_spec usb_drv_ep_specs[USB_NUM_ENDPOINTS] = {
    [0] = {{USB_ENDPOINT_XFER_CONTROL, USB_ENDPOINT_XFER_CONTROL}},
    [1] = {{USB_ENDPOINT_TYPE_NONE, USB_ENDPOINT_TYPE_ANY}},
    [2] = {{USB_ENDPOINT_TYPE_ANY, USB_ENDPOINT_TYPE_NONE}},
    [3] = {{USB_ENDPOINT_TYPE_NONE, USB_ENDPOINT_TYPE_ANY}},
    [4] = {{USB_ENDPOINT_TYPE_ANY, USB_ENDPOINT_TYPE_NONE}},
    [5] = {{USB_ENDPOINT_TYPE_NONE, USB_ENDPOINT_TYPE_ANY}},
};
uint8_t usb_drv_ep_specs_flags = USB_ENDPOINT_SPEC_IO_EXCLUSIVE;

static int ep_mps(int ep_num)
{
    if (ep_num == 0)
        return EP0_MPS;
    return high_speed ? 512 : 64;
}

/* FIFO access, one packet */

static void fifo_write(int ep_num, const unsigned char *p, int n)
{
    int i;
    USB_BWCR = n;
    for (i = 0; i + 1 < n; i += 2)
        EP_FIFO(ep_num) = (p[i] << 8) | p[i + 1];
    if (n & 1)
        EP_FIFO(ep_num) = p[n - 1] << 8;
}

/* reads one packet of 'bytes' bytes, keeping at most 'room' */
static void fifo_read(int ep_num, unsigned char *p, int bytes, int room)
{
    int i;
    for (i = 0; i < bytes; i += 2) {
        unsigned int h = EP_FIFO(ep_num) & 0xffff;
        if (i < room)
            p[i] = h >> 8;
        if (i + 1 < bytes && i + 1 < room)
            p[i + 1] = h & 0xff;
    }
}

static void ep_complete(int ep_num, int dir, int result)
{
    struct ep_state *ep = &ep_states[ep_num][dir];
    if (!ep->busy)
        return;
    ep->busy = false;
    ep->result = result;
    usb_core_transfer_complete(ep_num, dir == DIR_IN ? USB_DIR_IN : USB_DIR_OUT,
                               result, ep->cnt);
    if (ep->block)
        semaphore_release(&ep->complete);
}

/* EP0 */

static void ep0_fail_transfers(void)
{
    /* OUT first: in a control read the core expects the status OUT to end
       before the data IN */
    ep_complete(0, DIR_OUT, -1);
    ep_complete(0, DIR_IN, -1);
}

static void ep0_send_packet(void)
{
    struct ep_state *ep = &ep_states[0][DIR_IN];
    int n = MIN(ep->len - ep->cnt, EP0_MPS);
    USB_IR = 0;
    fifo_write(0, ep->buf + ep->cnt, n);
    ep->cnt += n;
    if (n < EP0_MPS)
        ep->zlp = false;    /* a short packet ends it */
}

static void ep0_in_done(void)
{
    ep_complete(0, DIR_IN, 0);
    /* the controller does the status stage: an armed status OUT is done */
    struct ep_state *out = &ep_states[0][DIR_OUT];
    if (out->busy && out->len == 0)
        ep_complete(0, DIR_OUT, 0);
}

static void ep0_setup(void)
{
    union {
        unsigned char b[8];
        struct usb_ctrlrequest req;
    } s;

    fifo_read(0, s.b, 8, 8);
    USB_EP0SR = EP0SR_RX_SUCCESS;
    usb_dbg_setups++;
    dbg_ev("S", (s.b[0] << 8) | s.b[1]);

    /* a transfer still in flight belongs to a request the host gave up */
    ep0_fail_transfers();

    /* the USB core takes the fields in CPU byte order, the wire is little
       endian and the CPU big endian */
    s.req.wValue = letoh16(s.req.wValue);
    s.req.wIndex = letoh16(s.req.wIndex);
    s.req.wLength = letoh16(s.req.wLength);

    /* the controller handles SET_ADDRESS, status stage included */
    if (usb_drv_is_set_address(&s.req)) {
        usb_core_notify_set_address(s.req.wValue & 0x7f);
        return;
    }
    ep0_wlength = s.req.wLength;
    usb_core_setup_received(&s.req);
}

static void ep0_int(void)
{
    unsigned int sr;

    USB_IR = 0;
    sr = USB_EP0SR;
    usb_dbg_last_ep0sr = sr;
    dbg_ev("0:", sr);

    if (sr & EP0SR_STALL) {
        USB_EP0CR &= ~EP0CR_STALL;
        USB_EP0SR = EP0SR_STALL;
    }

    if (sr & EP0SR_TX_SUCCESS) {
        struct ep_state *ep = &ep_states[0][DIR_IN];
        USB_EP0SR = EP0SR_TX_SUCCESS;
        if (ep->busy) {
            if (ep->cnt < ep->len)
                ep0_send_packet();
            else if (ep->zlp) {
                ep->zlp = false;
                USB_IR = 0;
                USB_BWCR = 0;
            }
            else
                ep0_in_done();
        }
    }

    if (sr & EP0SR_RX_SUCCESS) {
        struct ep_state *ep = &ep_states[0][DIR_OUT];
        int halfwords = USB_BRCR & 0x3ff;
        int bytes = (sr & EP0SR_LWO) ? halfwords * 2 - 1 : halfwords * 2;

        if (ep->busy && ep->len > 0) {
            /* control OUT data stage */
            int room = ep->len - ep->cnt;
            fifo_read(0, ep->buf + ep->cnt, bytes, room);
            USB_EP0SR = EP0SR_RX_SUCCESS;
            ep->cnt += MIN(bytes, room);
            if (ep->cnt >= ep->len || bytes < EP0_MPS)
                ep_complete(0, DIR_OUT, 0);
        }
        else if (halfwords == 4) {
            ep0_setup();
        }
        else {
            /* status packet or junk: drop it */
            while (halfwords--)
                (void)EP_FIFO(0);
            USB_EP0SR = EP0SR_RX_SUCCESS;
            if (ep->busy && ep->len == 0)
                ep_complete(0, DIR_OUT, 0);
        }
    }
}

/* other endpoints */

static void ep_in_kick(int n)
{
    struct ep_state *ep = &ep_states[n][DIR_IN];
    int mps = ep_mps(n);
    int len = MIN(ep->len - ep->cnt, mps);

    USB_IR = n;
    dbg_ev("K", (n << 12) | len);
    fifo_write(n, ep->buf + ep->cnt, len);
    ep->cnt += len;
    if (len < mps)
        ep->zlp = false;
}

static void ep_in_int(int n)
{
    struct ep_state *ep = &ep_states[n][DIR_IN];
    unsigned int sr;

    USB_IR = n;
    sr = USB_ESR;
    dbg_ev("T", (n << 12) | sr);
    if (sr & ESR_STALL)
        USB_ESR = ESR_STALL;
    if (!(sr & ESR_TX_SUCCESS))
        return;
    USB_ESR = ESR_TX_SUCCESS;
    if (!ep->busy)
        return;
    if (ep->cnt < ep->len)
        ep_in_kick(n);
    else if (ep->zlp) {
        ep->zlp = false;
        USB_IR = n;
        USB_BWCR = 0;
    }
    else
        ep_complete(n, DIR_IN, 0);
}

/* reads the packets waiting in an OUT endpoint's FIFO into the armed
   transfer. Data that arrives with no transfer armed stays in the FIFO
   (the endpoint NAKs) until one is. */
static void ep_out_drain(int n)
{
    struct ep_state *ep = &ep_states[n][DIR_OUT];
    int mps = ep_mps(n);

    USB_IR = n;
    while (ep->busy) {
        unsigned int sr = USB_ESR;
        int halfwords, bytes, room;

        if (sr & ESR_STALL)
            USB_ESR = ESR_STALL;
        if (!(sr & ESR_RX_SUCCESS))
            break;
        halfwords = USB_BRCR & 0x3ff;
        bytes = (sr & ESR_LWO) ? halfwords * 2 - 1 : halfwords * 2;
        room = ep->len - ep->cnt;
        fifo_read(n, ep->buf + ep->cnt, bytes, room);
        USB_ESR = ESR_RX_SUCCESS;
        dbg_ev("O", (n << 12) | bytes);
        ep->cnt += MIN(bytes, room);
        if (ep->cnt >= ep->len || bytes < mps)
            ep_complete(n, DIR_OUT, 0);
    }
}

static unsigned int eier_mask = 1;     /* EP0 + allocated endpoints */
static unsigned int ep_mps_set[USB_NUM_ENDPOINTS];

/* controller set-up, also after every bus reset (the controller's
   registers are reset when the bus reset ends) */
static void udc_setup(void)
{
    int n;

    USB_EDR = 0x2a;             /* endpoints 1, 3, 5 IN */
    USB_TR = 0;
    /* the original firmware's value; s3c-hsudc uses DTZIEN|RRD|SUS|RST */
    USB_SCR = 0x4ec3;
    USB_EP0CR = 0;
    USB_IR = 0;
    USB_MPR = EP0_MPS;
    for (n = 1; n < USB_NUM_ENDPOINTS; n++) {
        if (ep_mps_set[n]) {
            USB_IR = n;
            USB_MPR = ep_mps(n);
        }
    }
    USB_IR = 0;
    USB_EIER = eier_mask;
}

static void bus_reset(void)
{
    int n;
    long spin = 0;

    usb_dbg_resets++;
    /* like the original firmware: wait for the end of the reset */
    while ((USB_SSR & SSR_RESET) && spin++ < 2000000)
        USB_SSR = SSR_RESET;
    dbg_ev("R", spin);
    udc_setup();

    for (n = 1; n < USB_NUM_ENDPOINTS; n++) {
        ep_complete(n, DIR_IN, -1);
        ep_complete(n, DIR_OUT, -1);
    }
    ep0_fail_transfers();
    usb_core_bus_reset();
}

void INT_USB_FUNC(void)
{
    unsigned int ssr = USB_SSR;
    unsigned int eir = USB_EIR & 0x1ff;
    int n;

    usb_dbg_irqs++;
    usb_dbg_last_ssr = ssr;

    if (ssr & SSR_ANY) {
        USB_SSR = ssr & SSR_ANY & ~SSR_RESET;
        if (ssr & SSR_SDE)
            high_speed = (ssr & SSR_HSP) != 0;
        if (ssr & SSR_RESET)
            bus_reset();
    }

    if (eir & 1) {
        USB_EIR = 1;
        ep0_int();
    }
    for (n = 1; n < USB_NUM_ENDPOINTS; n++) {
        if (!(eir & (1 << n)))
            continue;
        USB_EIR = 1 << n;
        if (n & 1)
            ep_in_int(n);
        else
            ep_out_drain(n);
    }
}

/* driver interface */

void usb_drv_startup(void)
{
}

void usb_drv_int_enable(bool enable)
{
    if (enable)
        INTMSK |= (1 << 16);
    else
        INTMSK &= ~(1 << 16);
}

void usb_drv_init(void)
{
    int n;

    usb_dbg_inits++;
    dbg_ev("I", usb_dbg_variant);
    for (n = 0; n < USB_NUM_ENDPOINTS; n++) {
        memset(ep_states[n], 0, sizeof(ep_states[n]));
        semaphore_init(&ep_states[n][DIR_OUT].complete, 1, 0);
        semaphore_init(&ep_states[n][DIR_IN].complete, 1, 0);
    }

    /* clock of the function controller */
    PWRCON &= ~(1 << 15);

    /* USB PHY power-up and reset, like the original firmware: after the
       cable was pulled, the PHY the boot ROM set up doesn't answer.
       usb_dbg_variant (debug screen): 0 = this, 1 = exactly the original
       firmware's sequence (PWRCON bit 14 ends up set), 2 = PHY untouched */
    if (usb_dbg_variant != 2) {
        PWRCON &= ~(1 << 14);
        USB_PHY_REG(0x10) = 4;
        USB_PHY_REG(0x04) = 0;
        USB_PHY_REG(0x00) = 0x10;
        USB_PHY_REG(0x08) |= 1;
        sleep(HZ / 100 + 1);
        USB_PHY_REG(0x08) &= ~1;
        sleep(HZ / 100 + 1);
    }
    if (usb_dbg_variant == 1) {
        PWRCON |= (1 << 14);
        PWRCON &= ~(1 << 14);
        USB_PHY_REG(0x00) = 0x10;
        USB_PHY_REG(0x04) = 0xff;
        PWRCON |= (1 << 14);
    }

    high_speed = (USB_SSR & SSR_HSP) != 0;
    eier_mask = 1;
    memset(ep_mps_set, 0, sizeof(ep_mps_set));
    udc_setup();
    USB_SSR = SSR_ANY;
    USB_EIR = 0x1ff;
    usb_drv_int_enable(true);
}

void usb_drv_exit(void)
{
    dbg_ev("X", 0);
    usb_drv_int_enable(false);
    USB_EIER = 0;
}

void usb_drv_ep_init(const struct usb_drv_ep_alloc_ctx* ctx, int ep)
{
    int n = EP_NUM(ep);
    (void)ctx;
    if (n == 0)
        return;
    dbg_ev("N", ep & 0xff);
    USB_IR = n;
    USB_MPR = ep_mps(n);
    USB_ECR = ECR_FLUSH;
    USB_ECR = 0;
    ep_mps_set[n] = 1;
    eier_mask |= 1 << n;
    USB_EIER = eier_mask;
}

void usb_drv_ep_deinit(const struct usb_drv_ep_alloc_ctx* ctx, int ep)
{
    int n = EP_NUM(ep);
    (void)ctx;
    if (n == 0)
        return;
    ep_mps_set[n] = 0;
    eier_mask &= ~(1 << n);
    USB_EIER = eier_mask;
}

int usb_drv_port_speed(void)
{
    return high_speed ? 1 : 0;
}

static int send_common(int endpoint, void *ptr, int length, bool block)
{
    int n = EP_NUM(endpoint);
    struct ep_state *ep = &ep_states[n][DIR_IN];
    int oldlevel;

    if (n == 0) {
        if (length == 0) {
            /* status IN of a control write: the controller sends it */
            oldlevel = disable_irq_save();
            ep->buf = NULL;
            ep->len = ep->cnt = 0;
            ep->busy = true;
            ep->block = false;
            ep_complete(0, DIR_IN, 0);
            restore_irq(oldlevel);
            return 0;
        }
        if (length > ep0_wlength)
            length = ep0_wlength;
    }

    if (block)
        semaphore_wait(&ep->complete, TIMEOUT_NOBLOCK);

    oldlevel = disable_irq_save();
    ep->buf = ptr;
    ep->len = length;
    ep->cnt = 0;
    ep->block = block;
    ep->result = 0;
    ep->busy = true;
    if (n == 0) {
        /* a short answer that fills whole packets ends with a ZLP */
        ep->zlp = length < ep0_wlength && (length % EP0_MPS) == 0;
        ep0_send_packet();
    }
    else {
        ep->zlp = false;
        ep_in_kick(n);
    }
    restore_irq(oldlevel);

    if (!block)
        return 0;
    if (semaphore_wait(&ep->complete, HZ) == OBJ_WAIT_TIMEDOUT) {
        oldlevel = disable_irq_save();
        bool pending = ep->busy;
        ep->busy = false;
        restore_irq(oldlevel);
        if (pending)
            return -1;
    }
    return ep->result;
}

int usb_drv_send(int endpoint, void *ptr, int length)
{
    return send_common(endpoint, ptr, length, true);
}

int usb_drv_send_nonblocking(int endpoint, void *ptr, int length)
{
    return send_common(endpoint, ptr, length, false);
}

int usb_drv_recv_nonblocking(int endpoint, void *ptr, int length)
{
    int n = EP_NUM(endpoint);
    if (n)
        dbg_ev("A", (n << 12) | (length & 0xfff));
    struct ep_state *ep = &ep_states[n][DIR_OUT];
    int oldlevel = disable_irq_save();

    ep->buf = ptr;
    ep->len = length;
    ep->cnt = 0;
    ep->block = false;
    ep->result = 0;
    ep->busy = true;
    /* EP0: a status OUT completes in ep0_in_done() or when the status
       packet arrives; other endpoints may have data waiting already */
    if (n != 0)
        ep_out_drain(n);
    restore_irq(oldlevel);
    return 0;
}

int usb_drv_recv_blocking(int endpoint, void *ptr, int length)
{
    (void)endpoint; (void)ptr; (void)length;
    return -1;
}

void usb_drv_stall(int endpoint, bool stall, bool in)
{
    int n = EP_NUM(endpoint);
    dbg_ev(stall ? "X1" : "X0", n);
    (void)in;

    USB_IR = n;
    if (n == 0) {
        if (stall)
            USB_EP0CR |= EP0CR_STALL;
        else
            USB_EP0CR &= ~EP0CR_STALL;
    }
    else {
        if (stall)
            USB_ECR |= ECR_STALL;
        else
            USB_ECR &= ~ECR_STALL;
    }
}

bool usb_drv_stalled(int endpoint, bool in)
{
    int n = EP_NUM(endpoint);
    (void)in;
    USB_IR = n;
    if (n == 0)
        return (USB_EP0CR & EP0CR_STALL) != 0;
    return (USB_ECR & ECR_STALL) != 0;
}

void usb_drv_reset_endpoint(int endpoint, bool send)
{
    int n = EP_NUM(endpoint);
    int oldlevel = disable_irq_save();
    ep_states[n][send ? DIR_IN : DIR_OUT].busy = false;
    if (n) {
        USB_IR = n;
        USB_ECR = ECR_FLUSH;
        USB_ECR = 0;
    }
    restore_irq(oldlevel);
}

void usb_drv_cancel_all_transfers(void)
{
    int n;
    int oldlevel = disable_irq_save();
    for (n = 1; n < USB_NUM_ENDPOINTS; n++) {
        struct ep_state *ep;
        int d;
        for (d = 0; d < 2; d++) {
            ep = &ep_states[n][d];
            if (ep->busy) {
                ep->busy = false;
                ep->result = -1;
                if (ep->block)
                    semaphore_release(&ep->complete);
            }
        }
    }
    restore_irq(oldlevel);
}

void usb_drv_set_test_mode(int mode)
{
    (void)mode;
}

bool usb_drv_powered(void)
{
    return (PDAT1 & (1 << 1)) != 0;
}

bool usb_drv_connected(void)
{
    return usb_drv_powered();
}

/* target glue */

void usb_init_device(void)
{
    /* P1.1 USB power detect input */
    PCON1 &= ~0x000000F0;
    usb_drv_startup();
}

void usb_attach(void)
{
    usb_enable(true);
}

void usb_enable(bool on)
{
    dbg_ev(on ? "E1" : "E0", 0);
    if (on)
        usb_core_init();
    else
        usb_core_exit();
}

int usb_detect(void)
{
    return (PDAT1 & (1 << 1)) ? USB_INSERTED : USB_EXTRACTED;
}

#else /* the bootloader has its own USB code (bootloader/samsung_yps3_usb.c) */

void usb_init_device(void)
{
    PCON1 &= ~0x000000F0;
}

void usb_enable(bool on)
{
    (void)on;
}

int usb_detect(void)
{
    return USB_EXTRACTED;
}

#endif
