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
 * @file obd_chip_report.c
 * @brief The chip configuration report (Ali, 2026-10-28, issue #1023): the
 *        read-only commands that show what the chip is and how it is set:
 *        its identity and firmware, the voltage it measures, the protocol,
 *        its CAN status counts, the programmable parameters (ATPPS: the
 *        EEPROM-backed settings the firmware provisions, PP 0E/0F among
 *        them), the sleep and wake summary (STSLCS), the power-pin switch
 *        (VTPPSWS). One block a user copies into a bug report. Through the
 *        request engine, inside one transaction (AutoPID yields), nothing
 *        written.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const struct
{
    const char *cmd;
    const char *what;
} REPORT_CMDS[OBD_REPORT_ITEMS] =
{
    { "ATI",     "chip"                         },
    { "AT@1",    "device description"           },
    { "AT@2",    "device identifier"            },
    { "VTVERS",  "firmware"                     },
    { "ATRV",    "voltage at the chip"          },
    { "ATDPN",   "protocol number"              },
    { "ATDP",    "protocol"                     },
    { "ATCS",    "CAN status counts"            },
    { "ATPPS",   "programmable parameters (EEPROM)" },
    { "STSLCS",  "sleep and wake configuration" },
    { "VTPPSWS", "power pin switch"             },
};

esp_err_t obd_chip_report_run(obd_chip_report_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->at_us = esp_timer_get_time();

    if (!obd_chip_ready() || obd_chip_update_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = obd_chip_txn_begin(pdMS_TO_TICKS(3000));

    if (err != ESP_OK)
    {
        return err;
    }

    /* the transaction's claim keeps AutoPID's requests out meanwhile; no
       client mark (it would make the CAN self-test refuse for 10 s) */

    for (size_t i = 0; i < OBD_REPORT_ITEMS; i++)
    {
        obd_chip_report_item_t *it = &out->items[i];

        strlcpy(it->cmd, REPORT_CMDS[i].cmd, sizeof(it->cmd));
        strlcpy(it->what, REPORT_CMDS[i].what, sizeof(it->what));
        it->err = obd_chip_request(it->cmd, it->resp, sizeof(it->resp),
                                   pdMS_TO_TICKS(1500));

        if (it->err != ESP_OK)
        {
            it->resp[0] = '\0';
        }

        /* the chip's line breaks become ours; a trailing one goes */
        size_t len = strlen(it->resp);

        for (size_t k = 0; k < len; k++)
        {
            if (it->resp[k] == '\r')
            {
                it->resp[k] = '\n';
            }
        }

        while (len > 0 && it->resp[len - 1] == '\n')
        {
            it->resp[--len] = '\0';
        }

        out->n++;
    }

    obd_chip_txn_end();
    return ESP_OK;
}
