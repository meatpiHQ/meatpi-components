/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file usb_host_manager_cli.c
 * @brief The `usb` CLI command (§6b).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_struct.h"
#include "soc/usb_wrap_struct.h"

#include "cmdline_manager.h"

#include "usb_host_manager.h"

/* WiCAN DEBUG (temporary): raw DWC2 port status for the eth bring-up.
 * HPRT = OTG base + 0x440; bit0 = PrtConnSts, bit2 = PrtEna,
 * bits[18:17] = PrtSpd. Remove after the bench passes. */
#define UHM_DBG_HPRT (*(volatile uint32_t *)(0x60080000u + 0x440u))

static int cmd_usb(int argc, char **argv)
{
    usb_host_manager_status_t st;

    /* WiCAN DEBUG (temporary): poke the mux/vbus pins live during the
     * eth bring-up — `usb mux <0|1>` / `usb vbus <0|1>` */
    if (argc >= 3 && strcmp(argv[1], "mux") == 0)
    {
        gpio_set_level(CONFIG_WICAN_USB_MODE_GPIO, atoi(argv[2]));
        cmdline_printf("mux io%d = %d\n", CONFIG_WICAN_USB_MODE_GPIO,
                       atoi(argv[2]));
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "vbus") == 0)
    {
        gpio_set_level(CONFIG_WICAN_SLEEP_USB_PWR_GPIO, atoi(argv[2]));
        cmdline_printf("vbus io%d = %d\n",
                       CONFIG_WICAN_SLEEP_USB_PWR_GPIO, atoi(argv[2]));
        return 0;
    }

    /* WiCAN DEBUG (temporary): DWC2 port suspend/resume — quiets the bus
     * (no SOF) without dropping enumeration. GPS-desense experiment
     * 2026-07-30: CherryUSB's HUB_PORT_FEATURE_SUSPEND is a no-op stub,
     * so poke HPRT directly. W1C bits (PCDET|PENA|PENCHNG|POCCHNG =
     * 0x2E) must be masked on write or the port disables itself. */
    if (argc >= 2 && strcmp(argv[1], "suspend") == 0)
    {
        uint32_t v = UHM_DBG_HPRT & ~0x2Eu;

        UHM_DBG_HPRT = v | 0x80u; /* PrtSusp */
        cmdline_printf("port suspended, HPRT=0x%08lx\n",
                       (unsigned long)UHM_DBG_HPRT);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "resume") == 0)
    {
        uint32_t v = UHM_DBG_HPRT & ~(0x2Eu | 0x80u);

        UHM_DBG_HPRT = v | 0x40u; /* PrtRes */
        vTaskDelay(pdMS_TO_TICKS(25));
        v = UHM_DBG_HPRT & ~(0x2Eu | 0x80u | 0x40u);
        UHM_DBG_HPRT = v;
        cmdline_printf("port resumed, HPRT=0x%08lx\n",
                       (unsigned long)UHM_DBG_HPRT);
        return 0;
    }

    (void)usb_host_manager_status(&st);

    cmdline_printf("usb host: %s, device %s, host mode %s\n",
                   st.enabled ? "enabled" : "disabled",
                   st.device_present ? "present" : "absent",
                   st.host_active ? "on" : "off");
    uint32_t hprt = UHM_DBG_HPRT;

    cmdline_printf("  HPRT=0x%08lx (conn=%lu ena=%lu spd=%lu)\n",
                   (unsigned long)hprt, (unsigned long)(hprt & 1u),
                   (unsigned long)((hprt >> 2) & 1u),
                   (unsigned long)((hprt >> 17) & 3u));
    cmdline_printf("  RTC usb_conf=0x%08lx  WRAP otg_conf=0x%08lx\n",
                   (unsigned long)RTCCNTL.usb_conf.val,
                   (unsigned long)USB_WRAP.otg_conf.val);
    cmdline_printf("  ethernet: %s%s%s (driver %s), attaches %lu\n",
                   st.eth_connected ? "up " : "down",
                   st.eth_connected ? st.ip : "",
                   st.eth_connected ? "" : "",
                   st.driver[0] ? st.driver : "-",
                   (unsigned long)st.attaches);
    return 0;
}

esp_err_t usb_host_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "usb",
        .help = "USB host / wired-uplink status",
        .func = cmd_usb,
    };

    return cmdline_manager_register(&CMD);
}
