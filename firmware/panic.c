/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by wavey@wavey.org
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "panic.h"
#include "lcd.h"
#include "font.h"
#include "debug.h"
#include "led.h"
#include "power.h"
#include "system.h"
#include "logf.h"
#include "rbversion.h"

#ifdef HAVE_RB_BACKTRACE
#include "gcc_extensions.h"
#include "backtrace.h"
#ifdef SAMSUNG_YPS3
#include "backlight-target.h"
#endif
#endif

#if (defined(CPU_MIPS) && (CONFIG_PLATFORM & PLATFORM_NATIVE))
/* TODO: see comment above exception_dump in system-mips.c */
char panic_buf[128];
#else
static char panic_buf[128];
#endif

#define LINECHARS (LCD_WIDTH/SYSFONT_WIDTH) - 2

#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
void panicf_f( const char *fmt, ...) USED_ATTR;

/* we wrap panicf() here with naked function to catch SP value */
void __attribute__((naked)) panicf( const char *fmt, ...)
{
    (void)fmt;
    asm volatile ("mov r4, sp \n"
                  "b panicf_f \n"
                 );
    while (1);
}

/*
 * "Dude. This is pretty fucked-up, right here."
 */
void panicf_f( const char *fmt, ...)
{
    int sp;

    asm volatile ("mov %[SP],r4 \n"
                  : [SP] "=r" (sp)
                 );

    int pc = (int)__builtin_return_address(0);
#elif defined(BACKTRACE_MIPSUNWINDER)
void panicf( const char *fmt, ... )
{
    /* NOTE: these are obtained by the backtrace lib */
    const int pc = 0;
    const int sp = 0;
#else
void panicf( const char *fmt, ...)
{
#endif
    va_list ap;

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    /* Disable interrupts */
#if defined(CPU_ARM_CLASSIC)
    disable_interrupt(IRQ_FIQ_STATUS);
#else
    set_irq_level(DISABLE_INTERRUPTS);
#endif
#endif /* SIMULATOR */

    va_start( ap, fmt );
    vsnprintf( panic_buf, sizeof(panic_buf), fmt, ap );
    va_end( ap );

    lcd_set_viewport(NULL);

    int y = 1;

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif

    lcd_clear_display();
#ifdef SAMSUNG_YPS3
    backlight_hw_on();  /* the panic may happen with the backlight off */
#endif
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, (unsigned char *) "*PANIC* (" RBVERSION ")");
    {
        /* wrap panic line */
        int i, len = strlen(panic_buf);
        for (i=0; i<len; i+=LINECHARS) {
            unsigned char c = panic_buf[i+LINECHARS];
            panic_buf[i+LINECHARS] = 0;
            lcd_puts(1, y++, (unsigned char *)panic_buf+i);
            panic_buf[i+LINECHARS] = c;
        }
    }

#if defined(HAVE_RB_BACKTRACE)
    rb_backtrace(pc, sp, &y);
#if defined(SAMSUNG_YPS3) && !defined(BOOTLOADER)
    {
        /* the FTL writer's state, for storage related panics */
        extern int ftl_write_last_error;
        extern unsigned long ftl_stat_pages, ftl_stat_merges, ftl_stat_cxt,
            ftl_write_refused, ftl_write_refused_sector,
            ftl_stat_wcalls, ftl_stat_wlast;
        extern bool ftl_write_enabled;
        lcd_putsf(0, y++, "ftl wr %d err %d pg %lu mrg %lu cxt %lu",
                  ftl_write_enabled, ftl_write_last_error, ftl_stat_pages,
                  ftl_stat_merges, ftl_stat_cxt);
        lcd_putsf(0, y++, "refused %lu last %lu", ftl_write_refused,
                  ftl_write_refused_sector);
        lcd_putsf(0, y++, "calls %lu last %lu", ftl_stat_wcalls,
                  ftl_stat_wlast);
        extern unsigned long nand_ll_read_errors, ftl_stat_rerr;
        extern int nand_ll_read_last_err, ftl_stat_rerr_rc;
        extern unsigned int nand_ll_read_last_page;
        lcd_putsf(0, y++, "nand rderr %lu st %d pg %u",
                  nand_ll_read_errors, nand_ll_read_last_err,
                  nand_ll_read_last_page);
        lcd_putsf(0, y++, "ftl rderr %lu rc %d", ftl_stat_rerr,
                  ftl_stat_rerr_rc);
    }
#endif
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf_panic_dump(&y);
#endif

    lcd_update();
    DEBUGF("%s", panic_buf);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif

    system_exception_wait(); /* if this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}
