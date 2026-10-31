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
 * @file http_server_manager_audit.c
 * @brief The flash audit's target half (2026-10-31): the SPI flash
 *        counters as a snapshot, and the per-route table in PSRAM. Written
 *        by the server task only (the gate trampoline), read by the `http`
 *        console command without a lock: they are counters.
 */
#include "esp_attr.h"
#include "sdkconfig.h"

#if CONFIG_SPI_FLASH_ENABLE_COUNTERS
#include "esp_spi_flash_counters.h"
#endif

#include "http_server_manager_private.h"

static hsm_audit_row_t s_rows[HSM_AUDIT_ROWS] EXT_RAM_BSS_ATTR;

void hsm_audit_snapshot(hsm_flash_ops_t *out)
{
    if (out == NULL)
    {
        return;
    }

#if CONFIG_SPI_FLASH_ENABLE_COUNTERS
    const esp_flash_counters_t *c = esp_flash_get_counters();

    out->reads = c->read.count;
    out->writes = c->write.count;
    out->erases = c->erase.count;
#else
    out->reads = 0;
    out->writes = 0;
    out->erases = 0;
#endif
}

void hsm_audit_note(size_t idx, const hsm_flash_ops_t *before)
{
    hsm_flash_ops_t after;

    hsm_audit_snapshot(&after);
    hsm_audit_account(s_rows, HSM_AUDIT_ROWS, idx, before, &after);
}

const hsm_audit_row_t *hsm_audit_rows(size_t *count_out)
{
    if (count_out != NULL)
    {
        *count_out = HSM_AUDIT_ROWS;
    }

    return s_rows;
}
