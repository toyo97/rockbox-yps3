/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright © 2008 Rafaël Carré
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

#include <stdbool.h>
#include "config.h"
#include "adc.h"
#include "usb.h"
#include "power.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "storage.h"
#ifdef IPOD_NANO2G
#include "power.h"
#include "pmu-target.h"
#include "nand-target.h"
#endif
#ifdef HAVE_SERIAL
#include "uart-target.h"
#include "uc87xx.h"
#endif

#define DEBUG_CANCEL BUTTON_MENU

/*  Skeleton for adding target specific debug info to the debug menu
 */

#define _DEBUG_PRINTF(a, varargs...) lcd_putsf(0, line++, (a), ##varargs);

extern int lcd_type;
extern uint32_t nand_type[4];

bool dbg_hw_info(void)
{
    int line;
    int i;
#ifdef IPOD_NANO2G
    unsigned int state = 0;
#ifdef UC87XX_DEBUG
    const unsigned int max_states=3;
#else
    const unsigned int max_states=2;
#endif
    int nand_bank_count;
    struct storage_info info;
    const struct nand_device_info_type *nand_devicetype[4];
    nand_get_info(&info);
    nand_bank_count = 0;
    for(i=0;i<4;i++)
    {
        nand_devicetype[i] = nand_get_device_type(i);
        if(nand_devicetype[i] != NULL) nand_bank_count++;
    }
#endif

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);

#ifdef IPOD_NANO2G
    state=0;
#endif
    while(1)
    {
        lcd_clear_display();
        line = 0;

        /* _DEBUG_PRINTF statements can be added here to show debug info */
#ifdef IPOD_NANO2G

        if(state == 0)
        {
            _DEBUG_PRINTF("CPU:");
            _DEBUG_PRINTF("current_tick: %d", (unsigned int)current_tick);
            line++;

            _DEBUG_PRINTF("LCD:");
            _DEBUG_PRINTF("type: %d, %s", lcd_type, lcd_type ? "(7) LDS176" : "(2) ILI9320");
            line++;

            _DEBUG_PRINTF("NAND:");
            _DEBUG_PRINTF("banks: %d",nand_bank_count);

            for(i=0;i<4;i++)
            {
                if(nand_devicetype[i] != NULL)
                {
                    _DEBUG_PRINTF("bank: %d, id: %08X", i, (unsigned int)(*nand_devicetype[i]).id);
                }
            }

            _DEBUG_PRINTF("sectors: %d", info.num_sectors);
            _DEBUG_PRINTF("sector size: %d", info.sector_size);
            _DEBUG_PRINTF("last disk activity: %d", (unsigned int)nand_last_disk_activity());
        }
        else if(state==1)
        {
            _DEBUG_PRINTF("PMU:");
            for(i=0;i<7;i++)
            {
                static const char * const device[] = {"(unknown)", 
                                  "(CLICKWHEEL)", 
                                  "(LCD)",
                                  "(AUDIO)",
                                  "(NAND)",
                                  "(unknown)",
                                  "(ACCESSORY)"};
                _DEBUG_PRINTF("ldo%d %s: %dmV %s",i,
                    pmu_read(0x2e + (i << 1))?" on":"off",
                    900 + pmu_read(0x2d + (i << 1))*100,
                    device[i]);
            }
            _DEBUG_PRINTF("cpu voltage: %dmV",625 + pmu_read(0x1e)*25);
            _DEBUG_PRINTF("memory voltage: %dmV",625 + pmu_read(0x22)*25);
            line++;
            _DEBUG_PRINTF("charging: %s", charging_state() ? "true" : "false");
            _DEBUG_PRINTF("backlight: %s", pmu_read(0x29) ? "on" : "off");
            _DEBUG_PRINTF("brightness value: %d", pmu_read(0x28));
        }
#ifdef UC87XX_DEBUG
        else if(state==2)
        {
            extern struct uartc_port ser_port;
            int tx_stat, rx_stat, tx_speed, rx_speed;
            char line_cfg[4];
            int abr_stat;
            uint32_t abr_cnt;
            static const char * const abrstatus[] = {"Idle", "Launched", "Counting", "Abnormal"};

            uartc_port_get_line_info(&ser_port,
                        &tx_stat, &rx_stat, &tx_speed, &rx_speed, line_cfg);
            abr_stat = uartc_port_get_abr_info(&ser_port, &abr_cnt);

            _DEBUG_PRINTF("UART %d:", ser_port.id);
            line++;
            _DEBUG_PRINTF("line: %s", line_cfg);
            _DEBUG_PRINTF("Tx: %s, speed: %d", tx_stat ? "On":"Off", tx_speed);
            _DEBUG_PRINTF("Rx: %s, speed: %d", rx_stat ? "On":"Off", rx_speed);
            _DEBUG_PRINTF("ABR: %s, cnt: %u", abrstatus[abr_stat], abr_cnt);
            line++;
            _DEBUG_PRINTF("n_tx_bytes: %u", ser_port.n_tx_bytes);
            _DEBUG_PRINTF("n_rx_bytes: %u", ser_port.n_rx_bytes);
            _DEBUG_PRINTF("n_ovr_err: %u", ser_port.n_ovr_err);
            _DEBUG_PRINTF("n_parity_err: %u", ser_port.n_parity_err);
            _DEBUG_PRINTF("n_frame_err: %u", ser_port.n_frame_err);
            _DEBUG_PRINTF("n_break_detect: %u", ser_port.n_break_detect);
            _DEBUG_PRINTF("ABR n_abnormal: %u %u",
                        ser_port.n_abnormal0, ser_port.n_abnormal1);
        }
#endif
        else
        {
            state=0;
        }

#else
        _DEBUG_PRINTF("dbg_hw_info");
        _DEBUG_PRINTF("current_tick: %d", (unsigned int)current_tick);
#ifdef SAMSUNG_YPS3
        for (i = 0; i < 4; i++) {
            _DEBUG_PRINTF("ADC %d: %4d", i, adc_read(i));
        }
        _DEBUG_PRINTF("USB power: %s", (PDAT1 & 2) ? "yes" : "no");
        _DEBUG_PRINTF("P5: %02x (charging %s)", (unsigned)PDAT5,
                      charging_state() ? "yes" : "no");
        _DEBUG_PRINTF("CPU: %ld MHz", cpu_frequency / 1000000);
        {
            /* time of a full LCD update, measured once (20 frames) */
            static long lcd_ms = -1;
            extern long ftl_stat_tmax[3];
            extern unsigned long ftl_stat_slow;
            if (lcd_ms < 0) {
                long t = current_tick;
                for (i = 0; i < 20; i++) {
                    lcd_update();
                }
                lcd_ms = (current_tick - t) * 1000 / HZ / 20;
            }
            _DEBUG_PRINTF("LCD %ld ms/frame", lcd_ms);
            _DEBUG_PRINTF("FTL max ms r %ld w %ld s %ld slow %lu",
                          ftl_stat_tmax[0] * 1000 / HZ,
                          ftl_stat_tmax[1] * 1000 / HZ,
                          ftl_stat_tmax[2] * 1000 / HZ, ftl_stat_slow);
        }
#ifdef HAVE_RECORDING
        {
            extern unsigned long rec_dbg_chunks, rec_dbg_nonzero;
            extern unsigned int rec_dbg_status;
            extern short rec_dbg_min, rec_dbg_max, rec_dbg_first[4];
            _DEBUG_PRINTF("rec n %lu nz %lu/256 st %x", rec_dbg_chunks,
                          rec_dbg_nonzero, rec_dbg_status);
            _DEBUG_PRINTF("rec %d..%d %04x %04x %04x %04x", rec_dbg_min,
                          rec_dbg_max, (unsigned short)rec_dbg_first[0],
                          (unsigned short)rec_dbg_first[1],
                          (unsigned short)rec_dbg_first[2],
                          (unsigned short)rec_dbg_first[3]);
        }
#endif
        {
            extern unsigned long tkey_words, tkey_bad, tkey_multi;
            extern int tkey_hist[8];
            _DEBUG_PRINTF("tkey n %lu bad %lu multi %lu btn %03x", tkey_words,
                          tkey_bad, tkey_multi, button_status());
            _DEBUG_PRINTF("%05x %05x %05x %05x", tkey_hist[0], tkey_hist[1],
                          tkey_hist[2], tkey_hist[3]);
            _DEBUG_PRINTF("%05x %05x %05x %05x", tkey_hist[4], tkey_hist[5],
                          tkey_hist[6], tkey_hist[7]);
        }
#ifdef HAVE_USBSTACK
        {
            extern unsigned long usb_dbg_irqs, usb_dbg_resets, usb_dbg_setups,
                usb_dbg_inits, usb_dbg_last_ssr, usb_dbg_last_ep0sr;
            extern int usb_dbg_variant, usb_dbg_ev_n;
            extern char usb_dbg_ev[64][10];
            int k;
            _DEBUG_PRINTF("var %d init %lu irq %lu", usb_dbg_variant,
                          usb_dbg_inits, usb_dbg_irqs);
            _DEBUG_PRINTF("rst %lu setup %lu", usb_dbg_resets, usb_dbg_setups);
            _DEBUG_PRINTF("SSR %04lx EP0SR %02lx", (unsigned long)USB_SSR & 0xffff,
                          (unsigned long)USB_EP0SR & 0xff);
            _DEBUG_PRINTF("EIR %03lx EIER %03lx", (unsigned long)USB_EIR & 0x1ff,
                          (unsigned long)USB_EIER & 0x1ff);
            _DEBUG_PRINTF("INTMSK %08lx", (unsigned long)INTMSK);
            /* the first 64 events after plugging in, 4 per line */
            for (k = 0; k < usb_dbg_ev_n && k < 64; k += 4) {
                _DEBUG_PRINTF("%-8s%-8s%-8s%-8s",
                    usb_dbg_ev[k],
                    k + 1 < usb_dbg_ev_n ? usb_dbg_ev[k + 1] : "",
                    k + 2 < usb_dbg_ev_n ? usb_dbg_ev[k + 2] : "",
                    k + 3 < usb_dbg_ev_n ? usb_dbg_ev[k + 3] : "");
            }
        }
#endif
        {
            extern bool ftl_write_enabled;
            extern int ftl_write_last_error;
            extern unsigned long ftl_stat_pages, ftl_stat_merges,
                ftl_stat_copymerges, ftl_stat_erases, ftl_write_refused,
                ftl_write_refused_sector;
            _DEBUG_PRINTF("FTL wr %d err %d pg %lu mrg %lu/%lu er %lu",
                          ftl_write_enabled, ftl_write_last_error,
                          ftl_stat_pages, ftl_stat_merges,
                          ftl_stat_copymerges, ftl_stat_erases);
            _DEBUG_PRINTF("refused %lu last %lu", ftl_write_refused,
                          ftl_write_refused_sector);
        }
#else
        (void)i;
#endif
#endif

        lcd_update(); 
        switch(button_get_w_tmo(HZ/20))
        {
#ifdef IPOD_NANO2G
            case BUTTON_SCROLL_BACK:
                if(state!=0) state--;
                break;

            case BUTTON_SCROLL_FWD:
                if(state!=max_states-1)
                {
                    state++;
                }
                break;
#endif

#if defined(SAMSUNG_YPS3) && defined(HAVE_USBSTACK)
            case SYS_USB_CONNECTED:
                /* stay on this screen while in USB mode, to watch it */
                usb_acknowledge(SYS_USB_CONNECTED_ACK, button_get_data());
                break;
#endif
#ifdef SAMSUNG_YPS3
#ifdef HAVE_USBSTACK
            case BUTTON_UP:
            {
                extern int usb_dbg_variant;
                usb_dbg_variant = (usb_dbg_variant + 1) % 3;
                break;
            }
#endif
#endif
            case DEBUG_CANCEL:
            case BUTTON_REL:
                lcd_setfont(FONT_UI);
                return false;
        }
    }

    lcd_setfont(FONT_UI);
    return false;
}

bool dbg_ports(void)
{
    int line;

    lcd_setfont(FONT_SYSFIXED);

    while(1)
    {
        lcd_clear_display();
        line = 0;
        
        _DEBUG_PRINTF("GPIO  0: %08x",(unsigned int)PDAT0);
        _DEBUG_PRINTF("GPIO  1: %08x",(unsigned int)PDAT1);
        _DEBUG_PRINTF("GPIO  2: %08x",(unsigned int)PDAT2);
        _DEBUG_PRINTF("GPIO  3: %08x",(unsigned int)PDAT3);
        _DEBUG_PRINTF("GPIO  4: %08x",(unsigned int)PDAT4);
        _DEBUG_PRINTF("GPIO  5: %08x",(unsigned int)PDAT5);
        _DEBUG_PRINTF("GPIO  6: %08x",(unsigned int)PDAT6);
        _DEBUG_PRINTF("GPIO  7: %08x",(unsigned int)PDAT7);
        _DEBUG_PRINTF("GPIO 10: %08x",(unsigned int)PDAT10);
        _DEBUG_PRINTF("GPIO 11: %08x",(unsigned int)PDAT11);
        _DEBUG_PRINTF("GPIO 13: %08x",(unsigned int)PDAT13);
        _DEBUG_PRINTF("GPIO 14: %08x",(unsigned int)PDAT14);
        _DEBUG_PRINTF("5USEC  : %08x",(unsigned int)FIVE_USEC_TIMER);
        _DEBUG_PRINTF("USEC   : %08x",(unsigned int)USEC_TIMER);
        _DEBUG_PRINTF("USECREG: %08x",(unsigned int)(*(REG32_PTR_T)(0x3C700084)));

        lcd_update();
        if (button_get_w_tmo(HZ/10) == (DEBUG_CANCEL|BUTTON_REL))
            break;
    }
    lcd_setfont(FONT_UI);
    return false;
}

