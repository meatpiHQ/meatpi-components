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
 * @file http_server_manager_audit_core.c
 * @brief The flash audit's pure half (host-tested): a request's flash
 *        operations (the SPI flash counters after minus before) accounted
 *        to its route's row. A row index past the table lands on the last
 *        row, the catch-all's.
 */
#include "http_server_manager_private.h"

void hsm_audit_account(hsm_audit_row_t *rows, size_t row_count, size_t idx,
                       const hsm_flash_ops_t *before,
                       const hsm_flash_ops_t *after)
{
    if (rows == NULL || row_count == 0 || before == NULL || after == NULL)
    {
        return;
    }

    if (idx >= row_count)
    {
        idx = row_count - 1;
    }

    hsm_audit_row_t *r = &rows[idx];

    r->requests++;
    /* the counters only grow; the unsigned difference survives a wrap */
    r->ops.reads += after->reads - before->reads;
    r->ops.writes += after->writes - before->writes;
    r->ops.erases += after->erases - before->erases;
}

bool hsm_audit_row_touched_flash(const hsm_audit_row_t *row)
{
    return row != NULL &&
           (row->ops.reads != 0 || row->ops.writes != 0 ||
            row->ops.erases != 0);
}
